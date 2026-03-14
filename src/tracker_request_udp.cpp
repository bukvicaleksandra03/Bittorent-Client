#include "tracker_request_udp.h"

#include <arpa/inet.h>
#include <endian.h>
#include <poll.h>

#include <cstring>

#include "logger.h"

// Big-endian helpers
static void write_be64(uint8_t* buf, uint64_t val)
{
    uint64_t be = htobe64(val);
    std::memcpy(buf, &be, sizeof(be));
}

static void write_be32(uint8_t* buf, uint32_t val)
{
    uint32_t be = htonl(val);
    std::memcpy(buf, &be, sizeof(be));
}

static void write_be16(uint8_t* buf, uint16_t val)
{
    uint16_t be = htons(val);
    std::memcpy(buf, &be, sizeof(be));
}

static uint64_t read_be64(const uint8_t* buf)
{
    uint64_t be;
    std::memcpy(&be, buf, sizeof(be));
    return be64toh(be);
}

static uint32_t read_be32(const uint8_t* buf)
{
    uint32_t be;
    std::memcpy(&be, buf, sizeof(be));
    return ntohl(be);
}

static uint16_t read_be16(const uint8_t* buf)
{
    uint16_t be;
    std::memcpy(&be, buf, sizeof(be));
    return ntohs(be);
}

TrackerRequestUDP::TrackerRequestUDP(const PeerId& peer_id,
                                     const std::unique_ptr<TorrentFile>& tf)
    : TrackerRequest(peer_id, tf)
{
    _tracker_port = tf->get_tracker_port();
    if (_tracker_port == 0)
    {
        throw std::runtime_error("Tracker port must be set for UDP trackers.");
    }
}

uint32_t TrackerRequestUDP::generate_transaction_id()
{
    return _rng();
}

ssize_t TrackerRequestUDP::send_recv(Socket& socket,
                                     Address& addr,
                                     const uint8_t* send_buf,
                                     size_t send_len,
                                     uint8_t* recv_buf,
                                     size_t recv_len,
                                     int timeout_ms)
{
    // Send packet
    int fd = socket.get_fd();
    ssize_t sent =
        sendto(fd, send_buf, send_len, 0, addr.sockaddr_ptr(), addr.size());
    if (sent < 0)
    {
        throw std::runtime_error("sendto() failed");
    }

    // Wait for response with timeout
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;

    int result = poll(&pfd, 1, timeout_ms);
    if (result == 0)
    {
        throw std::runtime_error("UDP timeout");
    }
    if (result < 0)
    {
        throw std::runtime_error("poll() failed");
    }

    // Receive response
    ssize_t received = recvfrom(fd, recv_buf, recv_len, 0, nullptr, nullptr);
    if (received < 0)
    {
        throw std::runtime_error("recvfrom() failed");
    }

    return received;
}

uint64_t TrackerRequestUDP::udp_connect(Socket& socket, Address& addr)
{
    LOG_D("UDP Connect phase");

    // Connect request: 16 bytes
    // 0-7:   protocol_id
    // 8-11:  action (0 = connect)
    // 12-15: transaction_id
    uint8_t request[16];
    write_be64(request, PROTOCOL_ID);
    write_be32(request + 8, ACTION_CONNECT);

    uint32_t tx_id = generate_transaction_id();
    write_be32(request + 12, tx_id);

    for (int attempt = 0; attempt < MAX_RETRIES; attempt++)
    {
        try
        {
            uint8_t response[16];
            ssize_t received =
                send_recv(socket, addr, request, 16, response, 16, TIMEOUT_MS);

            if (received < 16)
            {
                LOG_D("Short connect response, retrying...");
                continue;
            }

            uint32_t action = read_be32(response);
            uint32_t resp_tx_id = read_be32(response + 4);

            if (action == ACTION_ERROR)
            {
                throw std::runtime_error("Tracker returned error");
            }

            if (action != ACTION_CONNECT)
            {
                throw std::runtime_error("Invalid connect response");
            }

            if (resp_tx_id != tx_id)
            {
                LOG_D("Transaction ID mismatch, retrying...");
                continue;
            }

            uint64_t connection_id = read_be64(response + 8);
            LOG_D("Got connection_id: " + std::to_string(connection_id));
            return connection_id;
        }
        catch (const std::exception& e)
        {
            LOG_D("Connect attempt " + std::to_string(attempt + 1) +
                  " failed: " + e.what());
            if (attempt == MAX_RETRIES - 1)
            {
                throw;
            }
        }
    }

    throw std::runtime_error("UDP connect failed after retries");
}

TrackerResponse TrackerRequestUDP::udp_announce(Socket& socket,
                                                Address& addr,
                                                uint64_t connection_id)
{
    LOG_D("UDP Announce phase");

    // Announce request: 98 bytes
    uint8_t request[98];
    write_be64(request, connection_id);
    write_be32(request + 8, ACTION_ANNOUNCE);

    uint32_t tx_id = generate_transaction_id();
    write_be32(request + 12, tx_id);

    // info_hash (20 bytes)
    std::memcpy(request + 16, _info_hash.data(), 20);

    // peer_id (20 bytes)
    std::memcpy(request + 36, _peer_id.data(), 20);

    // downloaded, left, uploaded
    write_be64(request + 56, _downloaded);
    write_be64(request + 64, _left);
    write_be64(request + 72, _uploaded);

    // event (2 = started)
    write_be32(request + 80, static_cast<uint32_t>(Event::Started));

    // IP (0 = default)
    write_be32(request + 84, 0);

    // key (random)
    write_be32(request + 88, generate_transaction_id());

    // num_want
    write_be32(request + 92, 50);

    // port
    write_be16(request + 96, _peer_port);

    for (int attempt = 0; attempt < MAX_RETRIES; attempt++)
    {
        try
        {
            uint8_t response[2048];
            ssize_t received = send_recv(socket,
                                         addr,
                                         request,
                                         98,
                                         response,
                                         sizeof(response),
                                         TIMEOUT_MS);

            if (received < 20)
            {
                LOG_D("Short announce response, retrying...");
                continue;
            }

            uint32_t action = read_be32(response);
            uint32_t resp_tx_id = read_be32(response + 4);

            if (action == ACTION_ERROR)
            {
                std::string error(reinterpret_cast<char*>(response + 8),
                                  received - 8);
                return TrackerResponseUDP::failure("Tracker error: " + error);
            }

            if (action != ACTION_ANNOUNCE)
            {
                throw std::runtime_error("Invalid announce response");
            }

            if (resp_tx_id != tx_id)
            {
                LOG_D("Transaction ID mismatch, retrying...");
                continue;
            }

            // Parse response
            int32_t interval = static_cast<int32_t>(read_be32(response + 8));
            int32_t leechers = static_cast<int32_t>(read_be32(response + 12));
            int32_t seeders = static_cast<int32_t>(read_be32(response + 16));

            // Parse peers (6 bytes each)
            std::vector<Peer> peers;
            size_t offset = 20;
            while (offset + 6 <= static_cast<size_t>(received))
            {
                Peer p;
                p.ip = std::to_string(response[offset]) + "." +
                       std::to_string(response[offset + 1]) + "." +
                       std::to_string(response[offset + 2]) + "." +
                       std::to_string(response[offset + 3]);
                p.port = read_be16(response + offset + 4);
                peers.push_back(p);
                offset += 6;
            }

            LOG_D("Announce success: " + std::to_string(seeders) +
                  " seeders, " + std::to_string(leechers) + " leechers, " +
                  std::to_string(peers.size()) + " peers");

            return TrackerResponseUDP(
                interval, leechers, seeders, std::move(peers));
        }
        catch (const std::exception& e)
        {
            LOG_D("Announce attempt " + std::to_string(attempt + 1) +
                  " failed: " + e.what());
            if (attempt == MAX_RETRIES - 1)
            {
                throw;
            }
        }
    }

    throw std::runtime_error("UDP announce failed after retries");
}

TrackerResponse TrackerRequestUDP::send()
{
    LOG_D("Sending UDP tracker request to " + _tracker_hostname + ":" +
          std::to_string(_tracker_port));

    // DNS lookup for UDP
    auto addresses = dns_lookup(
        _tracker_hostname, std::to_string(_tracker_port), SOCK_DGRAM);

    if (addresses.empty())
    {
        throw std::runtime_error("Failed to resolve UDP tracker: " +
                                 _tracker_hostname);
    }

    // Try each address
    for (const auto& address : addresses)
    {
        try
        {
            LOG_D("Trying address: " + address->identifier);

            // Create UDP socket
            Socket socket(address->domain(), SOCK_DGRAM);

            // Connect phase
            uint64_t connection_id = udp_connect(socket, *address);

            // Announce phase
            return udp_announce(socket, *address, connection_id);
        }
        catch (const std::exception& e)
        {
            LOG_D("Failed: " + std::string(e.what()));
        }
    }

    throw std::runtime_error("Failed to contact UDP tracker");
}
