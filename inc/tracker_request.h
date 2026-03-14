#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "crypto.h"
#include "torrent_file.h"
#include "tracker_response.h"
#include "utils.h"

using PeerId = utils::PeerId;

// Abstract base class for tracker requests
class TrackerRequest
{
   public:
    TrackerRequest(const PeerId& peer_id,
                   const std::unique_ptr<TorrentFile>& tf);
    virtual ~TrackerRequest() = default;

    // Delete copy
    TrackerRequest(const TrackerRequest&) = delete;
    TrackerRequest& operator=(const TrackerRequest&) = delete;

    // Send request to tracker and get response
    virtual TrackerResponse send() = 0;

   protected:
    // Tracker connection info (from TorrentFile)
    std::string _tracker_hostname;
    uint16_t _tracker_port;
    std::string _tracker_path;
    TrackerProtocol _protocol;

    // Announce parameters
    crypto::SHA1Hash _info_hash;
    PeerId _peer_id;
    uint16_t _peer_port;
    uint64_t _downloaded;
    uint64_t _left;
    uint64_t _uploaded;
};
