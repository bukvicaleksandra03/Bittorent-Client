#pragma once

#include <cstdint>

namespace byte_order
{

inline void write_be16(uint8_t* buf, uint16_t val)
{
    buf[0] = static_cast<uint8_t>(val >> 8);
    buf[1] = static_cast<uint8_t>(val);
}

inline void write_be32(uint8_t* buf, uint32_t val)
{
    buf[0] = static_cast<uint8_t>(val >> 24);
    buf[1] = static_cast<uint8_t>(val >> 16);
    buf[2] = static_cast<uint8_t>(val >> 8);
    buf[3] = static_cast<uint8_t>(val);
}

inline void write_be64(uint8_t* buf, uint64_t val)
{
    buf[0] = static_cast<uint8_t>(val >> 56);
    buf[1] = static_cast<uint8_t>(val >> 48);
    buf[2] = static_cast<uint8_t>(val >> 40);
    buf[3] = static_cast<uint8_t>(val >> 32);
    buf[4] = static_cast<uint8_t>(val >> 24);
    buf[5] = static_cast<uint8_t>(val >> 16);
    buf[6] = static_cast<uint8_t>(val >> 8);
    buf[7] = static_cast<uint8_t>(val);
}

inline uint16_t read_be16(const uint8_t* buf)
{
    return static_cast<uint16_t>(buf[0]) << 8 |
           static_cast<uint16_t>(buf[1]);
}

inline uint32_t read_be32(const uint8_t* buf)
{
    return static_cast<uint32_t>(buf[0]) << 24 |
           static_cast<uint32_t>(buf[1]) << 16 |
           static_cast<uint32_t>(buf[2]) << 8 |
           static_cast<uint32_t>(buf[3]);
}

inline uint64_t read_be64(const uint8_t* buf)
{
    return static_cast<uint64_t>(buf[0]) << 56 |
           static_cast<uint64_t>(buf[1]) << 48 |
           static_cast<uint64_t>(buf[2]) << 40 |
           static_cast<uint64_t>(buf[3]) << 32 |
           static_cast<uint64_t>(buf[4]) << 24 |
           static_cast<uint64_t>(buf[5]) << 16 |
           static_cast<uint64_t>(buf[6]) << 8 |
           static_cast<uint64_t>(buf[7]);
}

}  // namespace byte_order
