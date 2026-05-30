#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <random>
#include <sstream>
#include <string>

namespace utils
{

using PeerId = std::array<uint8_t, 20>;

bool is_power_of_two(uint64_t n);
PeerId generate_peer_id();
std::string to_hex(const PeerId& peer_id);

/// Human-readable size using binary units (KiB, MiB, GiB, TiB).
inline std::string format_byte_size(uint64_t bytes)
{
    constexpr uint64_t kib = 1024;
    constexpr uint64_t mib = kib * 1024;
    constexpr uint64_t gib = mib * 1024;
    constexpr uint64_t tib = gib * 1024;

    auto with_unit = [](double value, const char* unit) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << value << ' ' << unit;
        return oss.str();
    };

    if (bytes < kib)
        return std::to_string(bytes) + " B";
    if (bytes < mib)
        return with_unit(static_cast<double>(bytes) / kib, "KiB");
    if (bytes < gib)
        return with_unit(static_cast<double>(bytes) / mib, "MiB");
    if (bytes < tib)
        return with_unit(static_cast<double>(bytes) / gib, "GiB");
    return with_unit(static_cast<double>(bytes) / tib, "TiB");
}

// Human-readable transfer rate, e.g. "5.20 MiB/s", "512.00 KiB/s".
inline std::string format_byte_rate(double bytes_per_sec)
{
    if (bytes_per_sec < 0.0)
        bytes_per_sec = 0.0;

    constexpr double kib = 1024.0;
    constexpr double mib = kib * 1024.0;
    constexpr double gib = mib * 1024.0;

    auto with_unit = [](double value, const char* unit) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << value << ' ' << unit;
        return oss.str();
    };

    if (bytes_per_sec < kib)
        return with_unit(bytes_per_sec, "B/s");
    if (bytes_per_sec < mib)
        return with_unit(bytes_per_sec / kib, "KiB/s");
    if (bytes_per_sec < gib)
        return with_unit(bytes_per_sec / mib, "MiB/s");
    return with_unit(bytes_per_sec / gib, "GiB/s");
}

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
