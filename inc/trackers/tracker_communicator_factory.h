#pragma once

#include <memory>

#include "logger.h"
#include "trackers/http_tracker_communicator.h"
#include "trackers/tracker_communicator.h"
#include "trackers/udp_tracker_communicator.h"

inline std::unique_ptr<TrackerCommunicator> create_communicator(
    const TrackerProtocol& protocol)
{
    if (protocol == TrackerProtocol::UDP)
        return std::make_unique<UDPTrackerCommunicator>();
    if (protocol == TrackerProtocol::HTTP || protocol == TrackerProtocol::HTTPS)
        return std::make_unique<HTTPTrackerCommunicator>();
    LOG_AND_THROW("Unknown tracker protocol: " + std::string(protocol.scheme));
}