#include <gtest/gtest.h>

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "bencode/bencode_parser.h"
#include "logger.h"
#include "peer.h"
#include "torrent_file.h"
#include "trackers/tracker_communicator.h"
#include "trackers/tracker_communicator_factory.h"
#include "utils.h"

namespace fs = std::filesystem;

static const fs::path TORRENT_DIR = "torrent_files/unparsed_torrents";

static constexpr uint32_t EVENT_NONE = 0;
static constexpr uint32_t EVENT_COMPLETED = 1;
static constexpr uint32_t EVENT_STARTED = 2;
static constexpr uint32_t EVENT_STOPPED = 3;

static constexpr uint16_t LISTEN_PORT = 6881;

static std::vector<std::string> discover_torrent_files()
{
    std::vector<std::string> paths;
    for (const auto& entry : fs::directory_iterator(TORRENT_DIR))
    {
        if (entry.path().extension() == ".torrent")
            paths.push_back(entry.path().string());
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

class TrackerCommunicatorTest : public ::testing::TestWithParam<std::string>
{
   protected:
    void SetUp() override
    {
        logger::Logger::instance().set_level(logger::Level::DEBUG);

        const std::string& path = GetParam();
        BencodeParser parser(path);
        torrent = parser.parse();
        ASSERT_NE(torrent, nullptr) << "Failed to parse: " << path;

        tracker = torrent->get_tracker();
        communicator = create_communicator(tracker.protocol);
        peer_id = utils::generate_peer_id();

        std::cout << "[  INFO  ] File:      " << path << std::endl;
        std::cout << "[  INFO  ] Tracker:   " << tracker.to_string()
                  << std::endl;
        std::cout << "[  INFO  ] Protocol:  " << tracker.protocol.scheme
                  << std::endl;
        std::cout << "[  INFO  ] Info hash: " << torrent->get_info_hash_hex()
                  << std::endl;
    }

    std::vector<Peer> do_announce(uint64_t downloaded,
                                  uint64_t left,
                                  uint64_t uploaded,
                                  uint32_t event)
    {
        return communicator->announce(tracker,
                                      torrent->get_info_hash(),
                                      peer_id,
                                      downloaded,
                                      left,
                                      uploaded,
                                      event,
                                      LISTEN_PORT);
    }

    void print_peers(const std::vector<Peer>& peers, size_t max = 10)
    {
        std::cout << "[  INFO  ] Received " << peers.size() << " peers"
                  << std::endl;
        for (size_t i = 0; i < std::min(peers.size(), max); ++i)
        {
            std::cout << "[  INFO  ]   peer " << i << ": "
                      << peers[i].to_string() << std::endl;
        }
    }

    std::unique_ptr<TorrentFile> torrent;
    TrackerDetails tracker;
    utils::PeerId peer_id{};
    std::unique_ptr<TrackerCommunicator> communicator;
};

TEST_P(TrackerCommunicatorTest, AnnounceStarted)
{
    auto peers = do_announce(0, torrent->get_total_size(), 0, EVENT_STARTED);
    print_peers(peers);
    EXPECT_GT(peers.size(), 0u) << "Expected at least one peer from started";
}

TEST_P(TrackerCommunicatorTest, AnnounceNone)
{
    auto peers = do_announce(1024, 1000000, 512, EVENT_NONE);
    print_peers(peers);
    EXPECT_GT(peers.size(), 0u)
        << "Expected at least one peer from re-announce";
}

TEST_P(TrackerCommunicatorTest, AnnounceCompleted)
{
    uint64_t total = torrent->get_total_size();
    auto peers = do_announce(total, 0, total / 2, EVENT_COMPLETED);
    print_peers(peers);
    EXPECT_GT(peers.size(), 0u)
        << "Expected at least one peer from completed announce";
}

TEST_P(TrackerCommunicatorTest, AnnounceStopped)
{
    auto peers = do_announce(0, 0, 0, EVENT_STOPPED);
    print_peers(peers);
}

TEST_P(TrackerCommunicatorTest, FullLifecycle)
{
    uint64_t total = torrent->get_total_size();

    std::cout << "\n--- Phase 1: started ---" << std::endl;
    auto peers = do_announce(0, total, 0, EVENT_STARTED);
    print_peers(peers);
    EXPECT_GT(peers.size(), 0u) << "started should return peers";

    // Phases 2-4 may be rejected by trackers that enforce the announce
    // interval.  Log the rejection but do not fail the test for it.

    std::cout << "\n--- Phase 2: none (mid-download) ---" << std::endl;
    try
    {
        peers = do_announce(total / 4, total * 3 / 4, total / 8, EVENT_NONE);
        print_peers(peers);
        EXPECT_GT(peers.size(), 0u) << "re-announce should return peers";
    }
    catch (const std::exception& e)
    {
        std::cout << "[  WARN  ] Phase 2 failed: " << e.what() << std::endl;
    }

    std::cout << "\n--- Phase 3: completed ---" << std::endl;
    try
    {
        peers = do_announce(total, 0, total / 2, EVENT_COMPLETED);
        print_peers(peers);
        EXPECT_GT(peers.size(), 0u) << "completed should return peers";
    }
    catch (const std::exception& e)
    {
        std::cout << "[  WARN  ] Phase 3 failed: " << e.what() << std::endl;
    }

    std::cout << "\n--- Phase 4: stopped ---" << std::endl;
    try
    {
        peers = do_announce(total, 0, total / 2, EVENT_STOPPED);
        print_peers(peers);
    }
    catch (const std::exception& e)
    {
        std::cout << "[  WARN  ] Phase 4 failed: " << e.what() << std::endl;
    }
}

static std::string torrent_name(
    const ::testing::TestParamInfo<std::string>& info)
{
    fs::path p(info.param);
    std::string name = p.stem().string();
    for (auto& c : name)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)))
            c = '_';
    }
    return name;
}

INSTANTIATE_TEST_SUITE_P(AllTorrents,
                         TrackerCommunicatorTest,
                         ::testing::ValuesIn(discover_torrent_files()),
                         torrent_name);
