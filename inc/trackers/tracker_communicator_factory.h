#pragma once

#include <memory>
#include <stdexcept>

#include "trackers/http_tracker_communicator.h"
#include "trackers/tracker_communicator.h"
#include "trackers/udp_tracker_communicator.h"

inline std::unique_ptr<TrackerCommunicator> create_communicator(
    const TrackerProtocol& protocol,
    std::shared_ptr<logger::Logger> logger = nullptr)
{
    if (protocol == TrackerProtocol::UDP)
        return std::make_unique<UDPTrackerCommunicator>(logger);
    if (protocol == TrackerProtocol::HTTP || protocol == TrackerProtocol::HTTPS)
        return std::make_unique<HTTPTrackerCommunicator>(logger);
    throw std::runtime_error("Unknown tracker protocol: " +
                             std::string(protocol.scheme));
}