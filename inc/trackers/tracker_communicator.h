#pragma once

#include <vector>

#include "crypto.h"
#include "peer.h"
#include "trackers/tracker_details.h"
#include "utils.h"

class TrackerCommunicator
{
   public:
    virtual ~TrackerCommunicator() = default;
    virtual std::vector<Peer> announce(const TrackerDetails& tracker,
                                       const crypto::SHA1Hash& info_hash,
                                       const utils::PeerId& my_peer_id,
                                       uint64_t downloaded,
                                       uint64_t left,
                                       uint64_t uploaded) = 0;
};

inline std::unique_ptr<TrackerCommunicator> create_communicator(
    const TrackerProtocol& protocol)
{
    if (protocol == TrackerProtocol::UDP)
        return std::make_unique<UDPTrackerCommunicator>();
    if (protocol == TrackerProtocol::HTTP || protocol == TrackerProtocol::HTTPS)
        return std::make_unique<HTTPTrackerCommunicator>();
    throw std::runtime_error("Unknown tracker protocol: " +
                             std::string(protocol.scheme));
}