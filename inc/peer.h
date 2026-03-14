#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct Peer
{
    std::string ip;
    uint16_t port;

    std::string to_string() const
    {
        return ip + ":" + std::to_string(port);
    }
};

// Parse compact peer format (6 bytes per peer: 4 IP + 2 port)
inline std::vector<Peer> parse_compact_peers(const std::string& data)
{
    std::vector<Peer> peers;

    for (size_t i = 0; i + 6 <= data.size(); i += 6)
    {
        Peer p;

        // First 4 bytes = IP address
        p.ip = std::to_string(static_cast<uint8_t>(data[i])) + "." +
               std::to_string(static_cast<uint8_t>(data[i + 1])) + "." +
               std::to_string(static_cast<uint8_t>(data[i + 2])) + "." +
               std::to_string(static_cast<uint8_t>(data[i + 3]));

        // Last 2 bytes = port (big-endian)
        p.port = (static_cast<uint8_t>(data[i + 4]) << 8) |
                 static_cast<uint8_t>(data[i + 5]);

        peers.push_back(p);
    }

    return peers;
}
