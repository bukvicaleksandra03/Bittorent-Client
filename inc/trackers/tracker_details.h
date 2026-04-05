#pragma once

#include "trackers/tracker_protocol.h"

struct TrackerDetails
{
    TrackerProtocol protocol;
    std::string hostname;
    uint16_t port;
    std::string path;

    std::string to_string() const
    {
        return std::string(protocol.scheme) + "://" + hostname + ":" +
               std::to_string(port) + path;
    }
};