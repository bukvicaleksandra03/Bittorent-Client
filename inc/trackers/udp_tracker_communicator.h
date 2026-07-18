#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <random>
#include <stdexcept>
#include <vector>

#include "byte_order.h"
#include "crypto.h"
#include "net/socket_addresses.h"
#include "trackers/tracker_communicator.h"
#include "utils.h"

class UDPTrackerCommunicator : public TrackerCommunicator
{
   public:
    explicit UDPTrackerCommunicator(
        std::shared_ptr<logger::Logger> logger = nullptr)
        : TrackerCommunicator(std::move(logger))
    {}

    std::vector<PeerAddress> announce(const TrackerDetails& tracker,
                               const crypto::SHA1Hash& info_hash,
                               const utils::PeerId& my_peer_id,
                               uint64_t downloaded,
                               uint64_t left,
                               uint64_t uploaded,
                               uint32_t event,
                               uint16_t port) override;

   private:
    std::vector<PeerAddress> try_announce_to(const Address& addr,
                                      const crypto::SHA1Hash& info_hash,
                                      const utils::PeerId& my_peer_id,
                                      uint64_t downloaded,
                                      uint64_t left,
                                      uint64_t uploaded,
                                      uint32_t event,
                                      uint16_t port);

    static uint32_t random_transaction_id()
    {
        static std::mt19937 gen(std::random_device{}());
        return gen();
    }
    // --- Connect phase (BEP 15) ----------------------------------------

    struct ConnectionReq
    {
        uint64_t protocol_id = 0x41727101980;  // magic constant
        uint32_t action = 0;                   // 0 = connect
        uint32_t transaction_id;               // need to choose it randomly

        std::vector<uint8_t> serialize() const
        {
            std::vector<uint8_t> buf(16);
            byte_order::write_be64(buf.data(), protocol_id);
            byte_order::write_be32(buf.data() + 8, action);
            byte_order::write_be32(buf.data() + 12, transaction_id);
            return buf;
        }
    };

    struct ConnectionResp
    {
        uint32_t action;
        uint32_t transaction_id;
        uint64_t connection_id;

        static ConnectionResp deserialize(const uint8_t* data, size_t len)
        {
            if (len < 16)
                throw std::runtime_error("Connection response too short");
            ConnectionResp r;
            r.action = byte_order::read_be32(data);
            r.transaction_id = byte_order::read_be32(data + 4);
            r.connection_id = byte_order::read_be64(data + 8);
            return r;
        }
    };

    // --- Announce phase (BEP 15) ----------------------------------------
    // Offset  Size    Field
    //  0      64      connection_id
    //  8      32      action          (1 = announce)
    // 12      32      transaction_id
    // 16      20      info_hash
    // 36      20      peer_id
    // 56      64      downloaded
    // 64      64      left
    // 72      64      uploaded
    // 80      32      event           (0=none 1=completed 2=started 3=stopped)
    // 84      32      IP address      (0 = default)
    // 88      32      key
    // 92      32      num_want        (-1 = default)
    // 96      16      port
    // Total: 98 bytes

    struct AnnounceReq
    {
        uint64_t connection_id;
        uint32_t action = 1;  // 1 = announce
        uint32_t transaction_id;
        crypto::SHA1Hash info_hash;
        utils::PeerId peer_id;
        uint64_t downloaded;
        uint64_t left;
        uint64_t uploaded;
        uint32_t event = 0;  // 0 = none
        uint32_t ip = 0;     // 0 = default
        uint32_t key;
        int32_t num_want = -1;  // -1 = default
        uint16_t port;

        std::vector<uint8_t> serialize() const
        {
            std::vector<uint8_t> buf(98);
            uint8_t* p = buf.data();

            byte_order::write_be64(p, connection_id);
            p += 8;
            byte_order::write_be32(p, action);
            p += 4;
            byte_order::write_be32(p, transaction_id);
            p += 4;
            std::copy(info_hash.begin(), info_hash.end(), p);
            p += 20;
            std::copy(peer_id.begin(), peer_id.end(), p);
            p += 20;
            byte_order::write_be64(p, downloaded);
            p += 8;
            byte_order::write_be64(p, left);
            p += 8;
            byte_order::write_be64(p, uploaded);
            p += 8;
            byte_order::write_be32(p, event);
            p += 4;
            byte_order::write_be32(p, ip);
            p += 4;
            byte_order::write_be32(p, key);
            p += 4;
            byte_order::write_be32(p, static_cast<uint32_t>(num_want));
            p += 4;
            byte_order::write_be16(p, port);

            return buf;
        }
    };

    // Response header: 20 bytes, followed by peer entries.
    // IPv4: 6-byte entries (4 IP + 2 port)
    // IPv6: 18-byte entries (16 IP + 2 port)
    //
    // Offset  Size    Field
    //  0      32      action          (1 = announce)
    //  4      32      transaction_id
    //  8      32      interval        (seconds between re-announces)
    // 12      32      leechers
    // 16      32      seeders
    // ??      32      peer IP
    // ??      16      peer port

    struct AnnounceResp
    {
        uint32_t action;
        uint32_t transaction_id;
        uint32_t interval;
        uint32_t leechers;
        uint32_t seeders;
        std::vector<PeerAddress> peers;

        static AnnounceResp deserialize(const uint8_t* data,
                                        size_t len,
                                        bool is_ipv6)
        {
            if (len < 20)
                throw std::runtime_error("Announce response too short");

            AnnounceResp r;
            r.action = byte_order::read_be32(data);
            r.transaction_id = byte_order::read_be32(data + 4);
            r.interval = byte_order::read_be32(data + 8);
            r.leechers = byte_order::read_be32(data + 12);
            r.seeders = byte_order::read_be32(data + 16);

            const uint8_t* peer_data = data + 20;
            size_t peer_bytes = len - 20;
            size_t stride = is_ipv6 ? 18 : 6;

            for (size_t i = 0; i + stride <= peer_bytes; i += stride)
            {
                PeerAddress p;
                if (is_ipv6)
                {
                    char buf[INET6_ADDRSTRLEN];
                    inet_ntop(AF_INET6, peer_data + i, buf, sizeof(buf));
                    p.ip = buf;
                    p.port = byte_order::read_be16(peer_data + i + 16);
                }
                else
                {
                    p.ip = std::to_string(peer_data[i]) + "." +
                           std::to_string(peer_data[i + 1]) + "." +
                           std::to_string(peer_data[i + 2]) + "." +
                           std::to_string(peer_data[i + 3]);
                    p.port = byte_order::read_be16(peer_data + i + 4);
                }
                r.peers.push_back(p);
            }

            return r;
        }
    };
};
