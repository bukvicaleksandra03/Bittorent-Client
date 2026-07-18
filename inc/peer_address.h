#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct PeerAddress
{
    std::string ip;
    uint16_t port;

    std::string to_string() const
    {
        return ip + ":" + std::to_string(port);
    }
};

// Compact peer format: 4-byte IPv4 + 2-byte port (network byte order).
std::string peer_to_compact(const std::string& ip, uint16_t port);
bool compact_to_peer(const std::string& data, size_t offset,
                     std::string& out_ip, uint16_t& out_port);

inline std::vector<PeerAddress> parse_compact_peers(const std::string& data)
{
    std::vector<PeerAddress> peers;

    for (size_t i = 0; i + 6 <= data.size(); i += 6)
    {
        PeerAddress p;
        if (compact_to_peer(data, i, p.ip, p.port))
            peers.push_back(std::move(p));
    }

    return peers;
}
