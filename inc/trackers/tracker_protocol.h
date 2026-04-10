#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include "logger.h"

struct TrackerProtocol
{
    std::string_view scheme;
    uint16_t default_port;

    bool operator==(const TrackerProtocol& other) const
    {
        return scheme == other.scheme;
    }
    bool operator!=(const TrackerProtocol& other) const
    {
        return scheme != other.scheme;
    }

    static const TrackerProtocol HTTP;
    static const TrackerProtocol HTTPS;
    static const TrackerProtocol UDP;

    static TrackerProtocol from_scheme(std::string_view s)
    {
        if (s == "http")
            return HTTP;
        if (s == "https")
            return HTTPS;
        if (s == "udp")
            return UDP;
        LOG_AND_THROW("Unknown tracker protocol: " + std::string(s));
    }
};

inline constexpr TrackerProtocol TrackerProtocol::HTTP  = {"http",  80};
inline constexpr TrackerProtocol TrackerProtocol::HTTPS = {"https", 443};
inline constexpr TrackerProtocol TrackerProtocol::UDP   = {"udp",   6969};
