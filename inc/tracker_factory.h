#pragma once

#include <memory>
#include <stdexcept>

#include "torrent_file.h"
#include "tracker_request.h"
#include "tracker_request_http.h"
#include "tracker_request_udp.h"

// Factory function to create the appropriate tracker request
inline std::unique_ptr<TrackerRequest> create_tracker_request(
    const utils::PeerId& peer_id, const std::unique_ptr<TorrentFile>& tf)
{
    switch (tf->get_tracker_protocol())
    {
        case TrackerProtocol::UDP:
            return std::make_unique<TrackerRequestUDP>(peer_id, tf);

        case TrackerProtocol::HTTP:
        case TrackerProtocol::HTTPS:
            return std::make_unique<TrackerRequestHTTP>(peer_id, tf);

        default:
            throw std::runtime_error("Unknown tracker protocol");
    }
}
