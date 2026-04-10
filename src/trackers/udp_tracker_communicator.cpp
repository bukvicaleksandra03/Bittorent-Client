#include "trackers/udp_tracker_communicator.h"

#include <poll.h>

#include <string>

#include "byte_order.h"
#include "logger.h"
#include "net/socket.h"
#include "trackers/tracker_protocol.h"

static constexpr int MAX_RETRANSMISSIONS = 8;

static bool poll_readable(int fd, int timeout_ms)
{
    struct pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;

    int ret = ::poll(&pfd, 1, timeout_ms);
    return ret > 0 && (pfd.revents & POLLIN);
}

std::vector<Peer> UDPTrackerCommunicator::announce(
    const TrackerDetails& tracker,
    const crypto::SHA1Hash& info_hash,
    const utils::PeerId& my_peer_id,
    uint64_t downloaded,
    uint64_t left,
    uint64_t uploaded,
    uint32_t event,
    uint16_t port)
{
    if (tracker.protocol != TrackerProtocol::UDP)
    {
        LOG_AND_THROW(
            "UDPTrackerCommunicator used with non-UDP tracker: " +
            tracker.to_string());
    }

    LOG_I("UDP announce to " + tracker.to_string());

    auto addresses = dns_lookup(
        tracker.hostname, std::to_string(tracker.port), SOCK_DGRAM);
    if (addresses.empty())
    {
        LOG_AND_THROW(
            "DNS lookup returned no results for " + tracker.hostname);
    }

    std::string last_error;

    for (auto& addr_ptr : addresses)
    {
        const char* family =
            addr_ptr->domain() == AF_INET6 ? "IPv6" : "IPv4";
        LOG_D("Trying " + std::string(family) + " address for " +
              tracker.hostname);

        try
        {
            return try_announce_to(
                *addr_ptr, info_hash, my_peer_id, downloaded, left, uploaded,
                event, port);
        }
        catch (const std::exception& e)
        {
            last_error = e.what();
            LOG_W("Failed on " + std::string(family) + " address: " +
                  last_error);
        }
    }

    LOG_AND_THROW(
        "UDP announce failed on all resolved addresses for " +
        tracker.hostname + ": " + last_error);
}

std::vector<Peer> UDPTrackerCommunicator::try_announce_to(
    const Address& tracker_addr,
    const crypto::SHA1Hash& info_hash,
    const utils::PeerId& my_peer_id,
    uint64_t downloaded,
    uint64_t left,
    uint64_t uploaded,
    uint32_t event,
    uint16_t port)
{
    bool is_ipv6 = tracker_addr.domain() == AF_INET6;
    UDPClientSocket sock(tracker_addr.domain());

    // ---- BEP 15 Connect ------------------------------------------------

    ConnectionReq conn_req;
    conn_req.transaction_id = random_transaction_id();

    auto conn_buf = conn_req.serialize();
    std::string conn_msg(conn_buf.begin(), conn_buf.end());

    ConnectionResp conn_resp{};
    bool connected = false;

    for (int n = 0; n <= MAX_RETRANSMISSIONS; ++n)
    {
        int timeout_ms = 15000 * (1 << n);

        LOG_D("Connect attempt " + std::to_string(n) +
              " (timeout " + std::to_string(timeout_ms / 1000) + "s)");

        sock.sendto(conn_msg, tracker_addr);

        if (!poll_readable(sock.get_fd(), timeout_ms))
            continue;

        auto [data, src] = sock.recvfrom();
        auto raw = reinterpret_cast<const uint8_t*>(data.data());

        if (data.size() < 4)
            continue;

        uint32_t action = byte_order::read_be32(raw);
        if (action == 3)
        {
            std::string err_msg(data.begin() + 8, data.end());
            LOG_AND_THROW("Tracker error (connect): " + err_msg);
        }

        if (data.size() < 16)
            continue;

        conn_resp = ConnectionResp::deserialize(raw, data.size());
        if (conn_resp.transaction_id != conn_req.transaction_id)
        {
            LOG_W("Connect response transaction_id mismatch, ignoring");
            continue;
        }
        if (conn_resp.action != 0)
        {
            LOG_AND_THROW(
                "Unexpected action in connect response: " +
                std::to_string(conn_resp.action));
        }

        connected = true;
        break;
    }

    if (!connected)
    {
        LOG_AND_THROW(
            "UDP tracker connect timed out after all retransmissions");
    }

    LOG_D("Connected, connection_id obtained");

    // ---- BEP 15 Announce ------------------------------------------------

    AnnounceReq ann_req;
    ann_req.connection_id = conn_resp.connection_id;
    ann_req.transaction_id = random_transaction_id();
    ann_req.info_hash = info_hash;
    ann_req.peer_id = my_peer_id;
    ann_req.downloaded = downloaded;
    ann_req.left = left;
    ann_req.uploaded = uploaded;
    ann_req.event = event;
    ann_req.key = random_transaction_id();
    ann_req.port = port;

    auto ann_buf = ann_req.serialize();
    std::string ann_msg(ann_buf.begin(), ann_buf.end());

    for (int n = 0; n <= MAX_RETRANSMISSIONS; ++n)
    {
        int timeout_ms = 15000 * (1 << n);

        LOG_D("Announce attempt " + std::to_string(n) +
              " (timeout " + std::to_string(timeout_ms / 1000) + "s)");

        sock.sendto(ann_msg, tracker_addr);

        if (!poll_readable(sock.get_fd(), timeout_ms))
            continue;

        auto [data, src] = sock.recvfrom();
        auto raw = reinterpret_cast<const uint8_t*>(data.data());

        if (data.size() < 4)
            continue;

        uint32_t action = byte_order::read_be32(raw);
        if (action == 3)
        {
            std::string err_msg(data.begin() + 8, data.end());
            LOG_AND_THROW("Tracker error (announce): " + err_msg);
        }

        if (data.size() < 20)
            continue;

        auto ann_resp = AnnounceResp::deserialize(raw, data.size(), is_ipv6);
        if (ann_resp.transaction_id != ann_req.transaction_id)
        {
            LOG_W("Announce response transaction_id mismatch, ignoring");
            continue;
        }
        if (ann_resp.action != 1)
        {
            LOG_AND_THROW(
                "Unexpected action in announce response: " +
                std::to_string(ann_resp.action));
        }

        LOG_I("Announce OK  seeders=" + std::to_string(ann_resp.seeders) +
              " leechers=" + std::to_string(ann_resp.leechers) +
              " interval=" + std::to_string(ann_resp.interval) + "s" +
              " peers=" + std::to_string(ann_resp.peers.size()));

        return ann_resp.peers;
    }

    LOG_AND_THROW(
        "UDP tracker announce timed out after all retransmissions");
}
