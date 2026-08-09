// Manual tool: scrape HTTP/HTTPS trackers for every .torrent in
// torrent_files/unparsed_torrents/ and print swarm sizes.
//
// Build:  make test_scrape_all_torrents
// Run:    ./out/test_scrape_all_torrents
//
// Run from the project root (same as other tracker integration tests).

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "bencode/bencode_parser.h"
#include "logger.h"
#include "torrent_file.h"
#include "trackers/http_tracker_communicator.h"
#include "trackers/tracker_protocol.h"

namespace fs = std::filesystem;

namespace
{

static const fs::path TORRENT_DIR = "torrent_files/unparsed_torrents";

static std::vector<std::string> discover_torrent_files()
{
    std::vector<std::string> paths;
    if (!fs::is_directory(TORRENT_DIR))
    {
        return paths;
    }
    for (const auto& entry : fs::directory_iterator(TORRENT_DIR))
    {
        if (entry.path().extension() == ".torrent")
        {
            paths.push_back(entry.path().string());
        }
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

static bool is_scrapeable(const TrackerDetails& tracker)
{
    return tracker.protocol == TrackerProtocol::HTTP ||
           tracker.protocol == TrackerProtocol::HTTPS;
}

static void add_unique_tracker(std::vector<TrackerDetails>& out,
                               const TrackerDetails& tracker)
{
    if (!is_scrapeable(tracker))
    {
        return;
    }
    const std::string url = tracker.to_string();
    for (const auto& existing : out)
    {
        if (existing.to_string() == url)
        {
            return;
        }
    }
    out.push_back(tracker);
}

static std::vector<TrackerDetails> scrape_candidates_in_order(
    const TorrentFile& torrent)
{
    std::vector<TrackerDetails> candidates;
    add_unique_tracker(candidates, torrent.get_tracker());
    for (const auto& tier : torrent.get_announce_list())
    {
        for (const auto& tracker : tier)
        {
            add_unique_tracker(candidates, tracker);
        }
    }
    return candidates;
}

struct ScrapeSuccess
{
    TrackerDetails tracker;
    TrackerScrapeStats stats;
};

static std::optional<ScrapeSuccess> try_scrape(
    HTTPTrackerCommunicator& comm,
    const TorrentFile& torrent,
    std::string& last_error)
{
    const InfoHash& info_hash = torrent.get_info_hash();
    for (const TrackerDetails& tracker : scrape_candidates_in_order(torrent))
    {
        try
        {
            TrackerScrapeStats stats = comm.scrape(tracker, info_hash);
            return ScrapeSuccess{tracker, stats};
        }
        catch (const std::exception& e)
        {
            last_error = e.what();
        }
    }
    return std::nullopt;
}

static void print_header()
{
    std::cout << "filename\ttracker\tseeders\tleechers\tswarm\tdownloaded\n";
}

static void print_row(const fs::path& filename,
                      const std::string& tracker,
                      const TrackerScrapeStats& stats)
{
    const uint64_t swarm = stats.complete + stats.incomplete;
    std::cout << filename.filename().string() << '\t' << tracker << '\t'
              << stats.complete << '\t' << stats.incomplete << '\t' << swarm
              << '\t';
    if (stats.downloaded.has_value())
    {
        std::cout << *stats.downloaded;
    }
    else
    {
        std::cout << '-';
    }
    std::cout << '\n';
}

}  // namespace

TEST(ScrapeAllTorrents, ReportSwarmSizes)
{
    const std::vector<std::string> torrent_paths = discover_torrent_files();
    ASSERT_FALSE(torrent_paths.empty())
        << "No .torrent files found under " << TORRENT_DIR;

    auto log = std::make_shared<logger::Logger>();
    log->set_level(logger::Level::WARNING);
    HTTPTrackerCommunicator comm(log);

    print_header();

    for (const std::string& path : torrent_paths)
    {
        const fs::path file_path(path);
        bencode::Parser parser(path);
        std::unique_ptr<TorrentFile> torrent;
        try
        {
            torrent = parser.parse();
        }
        catch (const std::exception& e)
        {
            std::cout << file_path.filename().string()
                      << "\t(parse error)\t-\t-\t-\t-\n";
            std::cerr << "[ERROR] " << path << ": " << e.what() << '\n';
            continue;
        }

        const TrackerDetails primary = torrent->get_tracker();
        const bool primary_udp = primary.protocol == TrackerProtocol::UDP;
        const bool any_scrapeable =
            is_scrapeable(primary) ||
            std::any_of(torrent->get_announce_list().begin(),
                        torrent->get_announce_list().end(),
                        [](const std::vector<TrackerDetails>& tier) {
                            return std::any_of(
                                tier.begin(), tier.end(), is_scrapeable);
                        });

        if (!any_scrapeable)
        {
            std::cout << file_path.filename().string() << "\t(UDP-only)\t-\t-\t-\t-\n";
            if (primary_udp)
            {
                std::cerr << "[NOTE] " << path << ": primary tracker is UDP ("
                          << primary.to_string()
                          << "); HTTP scrape not supported\n";
            }
            continue;
        }

        std::string last_error;
        const std::optional<ScrapeSuccess> result =
            try_scrape(comm, *torrent, last_error);
        if (result.has_value())
        {
            print_row(file_path, result->tracker.to_string(), result->stats);
        }
        else
        {
            std::cout << file_path.filename().string()
                      << "\t(scrape failed)\t-\t-\t-\t-\n";
            std::cerr << "[ERROR] " << path;
            if (primary_udp)
            {
                std::cerr << " (primary UDP: " << primary.to_string() << ")";
            }
            std::cerr << ": " << last_error << '\n';
        }
    }
}
