#pragma once

#include "trackers/tracker_communicator.h"

class HTTPTrackerCommunicator : public TrackerCommunicator
{
   public:
    explicit HTTPTrackerCommunicator(
        std::shared_ptr<logger::Logger> logger = nullptr)
        : TrackerCommunicator(std::move(logger))
    {}

    std::vector<Peer> announce(const TrackerDetails& tracker,
                               const crypto::SHA1Hash& info_hash,
                               const utils::PeerId& my_peer_id,
                               uint64_t downloaded,
                               uint64_t left,
                               uint64_t uploaded,
                               uint32_t event,
                               uint16_t port) override;
};