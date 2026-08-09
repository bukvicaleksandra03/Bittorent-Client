#pragma once

#include <cstdint>
#include <optional>

struct TrackerScrapeStats
{
    // Seeders - Peers with the full torrent (100% downloaded)
    uint64_t complete = 0;

    // Leechers - Peers still downloading
    uint64_t incomplete = 0;

    // Total times the torrent has been completed (lifetime counter)
    std::optional<uint64_t> downloaded;
};
