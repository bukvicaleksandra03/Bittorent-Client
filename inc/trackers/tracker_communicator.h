#pragma once

#include <memory>
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
                                       uint64_t uploaded,
                                       uint32_t event,
                                       uint16_t port) = 0;

};