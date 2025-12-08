#include "utils.h"
namespace utils
{

bool is_power_of_two(uint64_t n)
{
    return n > 0 && (n & (n - 1)) == 0;
}

PeerId generate_peer_id()
{
    PeerId peer_id;

    // Client prefix: "-BT0100-" (8 bytes)
    const char* prefix = "-BT0100-";
    for (size_t i = 0; i < strlen(prefix); i++)
    {
        peer_id[i] = static_cast<uint8_t>(prefix[i]);
    }

    // Random suffix (12 bytes)
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);

    for (int i = 8; i < 20; ++i)
    {
        peer_id[i] = static_cast<uint8_t>(dis(gen));
    }

    return peer_id;
}

std::string to_hex(const PeerId& peer_id)
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