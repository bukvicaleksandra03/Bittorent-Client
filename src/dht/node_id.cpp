#include "dht/node_id.h"

#include <cstring>
#include <iomanip>
#include <random>
#include <sstream>

namespace dht
{

NodeId NodeId::random()
{
    NodeId id;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<unsigned int> dist(0, 255);
    for (auto& b : id.bytes)
        b = static_cast<uint8_t>(dist(gen));
    return id;
}

NodeId NodeId::from_string(const std::string& s)
{
    NodeId id;
    if (s.size() != 20)
        return id;  // bytes stay zero
    std::memcpy(id.bytes.data(), s.data(), 20);
    return id;
}

NodeId NodeId::from_info_hash(const InfoHash& hash)
{
    NodeId id;
    std::memcpy(id.bytes.data(), hash.bytes.data(), 20);
    return id;
}

std::string NodeId::to_string() const
{
    return std::string(reinterpret_cast<const char*>(bytes.data()), 20);
}

NodeId NodeId::operator^(const NodeId& other) const
{
    NodeId result;
    for (int i = 0; i < 20; ++i)
        result.bytes[i] = bytes[i] ^ other.bytes[i];
    return result;
}

bool NodeId::operator<(const NodeId& other) const
{
    return bytes < other.bytes;
}

bool NodeId::operator==(const NodeId& other) const
{
    return bytes == other.bytes;
}

bool NodeId::operator!=(const NodeId& other) const
{
    return bytes != other.bytes;
}

std::string NodeId::hex() const
{
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (auto b : bytes)
        oss << std::setw(2) << static_cast<unsigned>(b);
    return oss.str();
}

bool NodeId::is_zero() const
{
    for (auto b : bytes)
        if (b != 0)
            return false;
    return true;
}

}  // namespace dht
