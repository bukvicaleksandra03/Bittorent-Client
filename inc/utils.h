#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <random>
#include <sstream>

namespace utils
{

using PeerId = std::array<uint8_t, 20>;

bool is_power_of_two(uint64_t n);
PeerId generate_peer_id();
std::string to_hex(const PeerId& peer_id);

// ---------------------------------------------------------------------------

inline bool is_power_of_two(uint64_t n)
{
    return n > 0 && (n & (n - 1)) == 0;
}

inline PeerId generate_peer_id()
{
    PeerId peer_id;

    const char* prefix = "-BT0100-";
    for (size_t i = 0; i < strlen(prefix); i++)
    {
        peer_id[i] = static_cast<uint8_t>(prefix[i]);
    }

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);

    for (int i = 8; i < 20; ++i)
    {
        peer_id[i] = static_cast<uint8_t>(dis(gen));
    }

    return peer_id;
}

inline std::string to_hex(const PeerId& peer_id)
{
    std::stringstream ss;
    for (uint8_t byte : peer_id)
    {
        ss << std::hex << std::setw(2) << std::setfill('0')
           << static_cast<int>(byte);
    }
    return ss.str();
}

}  // namespace utils
