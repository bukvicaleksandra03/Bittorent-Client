#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct PeerAddress
{
    std::string ip;
    uint16_t port;

    PeerAddress(){};
    PeerAddress(std::string ip_, uint16_t port_) : ip(ip_), port(port_){};

    std::string to_string() const
    {
        return ip + ":" + std::to_string(port);
    }

    bool operator==(const PeerAddress& other) const
    {
        return port == other.port && ip == other.ip;
    }

    // Ordered by ip then port so PeerAddress can be used as a std::set/std::map
    // key.
    bool operator<(const PeerAddress& other) const
    {
        if (ip != other.ip)
            return ip < other.ip;
        return port < other.port;
    }
};

// Compact peer format: 4-byte IPv4 + 2-byte port (network byte order).
std::string peer_to_compact(const std::string& ip, uint16_t port);
bool compact_to_peer(const std::string& data,
                     size_t offset,
                     std::string& out_ip,
                     uint16_t& out_port);

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

namespace std
{
template <>
struct hash<PeerAddress>
{
    size_t operator()(const PeerAddress& pa) const noexcept
    {
        return hash<string>{}(pa.ip) ^ (static_cast<size_t>(pa.port) << 1);
    }
};
}  // namespace std
