#pragma once

#include "trackers/tracker_communicator.h"

class HTTPTrackerCommunicator : public TrackerCommunicator
{
   public:
    explicit HTTPTrackerCommunicator(
        std::shared_ptr<logger::Logger> logger = nullptr)
        : TrackerCommunicator(std::move(logger))
    {}

    std::vector<PeerAddress> announce(const TrackerDetails& tracker,
                               const InfoHash& info_hash,
                               const utils::PeerId& my_peer_id,
                               uint64_t downloaded,
                               uint64_t left,
                               uint64_t uploaded,
                               uint32_t event,
                               uint16_t port) override;

    TrackerScrapeStats scrape(const TrackerDetails& tracker,
                              const InfoHash& info_hash) override;
};

TrackerScrapeStats parse_http_scrape_response(const std::string& body,
                                              const InfoHash& info_hash);

TrackerDetails scrape_tracker_from_announce(const TrackerDetails& announce);