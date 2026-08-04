#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include "crypto.h"

// BEP-3 / BEP-5 torrent info_hash: exactly 20 raw bytes (SHA-1 of the bencoded
// info dict). Distinct from crypto::SHA1Hash so piece hashes and info hashes
// cannot be mixed accidentally.
struct InfoHash
{
    std::array<uint8_t, 20> bytes{};

    static InfoHash from_sha1(const crypto::SHA1Hash& hash)
    {
        InfoHash h;
        h.bytes = hash;
        return h;
    }

    // Accept only a 20-byte raw string (KRPC decode, register_torrent, etc.).
    static std::optional<InfoHash> try_from_raw(std::string_view raw)
    {
        if (raw.size() != 20)
            return std::nullopt;
        InfoHash h;
        std::memcpy(h.bytes.data(), raw.data(), 20);
        return h;
    }

    std::string to_raw() const
    {
        return std::string(reinterpret_cast<const char*>(bytes.data()), 20);
    }

    std::string hex() const
    {
        return crypto::to_hex(bytes);
    }

    std::string url_encoded() const
    {
        return crypto::url_encode(bytes);
    }

    bool operator==(const InfoHash& other) const
    {
        return bytes == other.bytes;
    }

    bool operator!=(const InfoHash& other) const
    {
        return !(*this == other);
    }
};

namespace std
{

template <>
struct hash<InfoHash>
{
    size_t operator()(const InfoHash& h) const noexcept
    {
        size_t result = 0;
        for (uint8_t b : h.bytes)
            result = result * 31 + b;
        return result;
    }
};

}  // namespace std
