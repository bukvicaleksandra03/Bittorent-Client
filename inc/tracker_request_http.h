#pragma once

#include "tracker_request.h"

// HTTP/HTTPS Tracker Request (BEP 3)
class TrackerRequestHTTP : public TrackerRequest
{
   public:
    TrackerRequestHTTP(const PeerId& peer_id,
                       const std::unique_ptr<TorrentFile>& tf);

    TrackerResponse send() override;

   private:
    std::string build_http_request() const;
};
