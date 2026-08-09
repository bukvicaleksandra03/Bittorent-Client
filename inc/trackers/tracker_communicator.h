#pragma once

#include <memory>
#include <stdexcept>
#include <vector>

#include "info_hash.h"
#include "logger.h"
#include "peer_address.h"
#include "trackers/tracker_details.h"
#include "trackers/tracker_scrape_stats.h"
#include "utils.h"

class TrackerCommunicator
{
   public:
    explicit TrackerCommunicator(
        std::shared_ptr<logger::Logger> logger = nullptr)
        : m_logger(std::move(logger))
    {}

    virtual ~TrackerCommunicator() = default;

    virtual std::vector<PeerAddress> announce(const TrackerDetails& tracker,
                                       const InfoHash& info_hash,
                                       const utils::PeerId& my_peer_id,
                                       uint64_t downloaded,
                                       uint64_t left,
                                       uint64_t uploaded,
                                       uint32_t event,
                                       uint16_t port) = 0;

    virtual TrackerScrapeStats scrape(const TrackerDetails& tracker,
                                      const InfoHash& /*info_hash*/)
    {
        throw std::runtime_error("Scrape is not supported for " +
                                 tracker.to_string());
    }

   protected:
    std::shared_ptr<logger::Logger> m_logger;
};