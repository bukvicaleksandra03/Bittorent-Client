#include <gtest/gtest.h>

#include <iostream>
#include <memory>
#include <string>

#include "bencode/bencode_parser.h"
#include "logger.h"
#include "peer.h"
#include "torrent_file.h"
#include "trackers/udp_tracker_communicator.h"
#include "utils.h"

static const std::string TORRENT_PATH =
    "torrent_files/unparsed_torrents/linuxmint-22.2-cinnamon-64bit.iso.torrent";

static constexpr uint32_t EVENT_NONE      = 0;
static constexpr uint32_t EVENT_COMPLETED = 1;
static constexpr uint32_t EVENT_STARTED   = 2;
static constexpr uint32_t EVENT_STOPPED   = 3;

static constexpr uint16_t LISTEN_PORT = 6881;

class UDPTrackerTest : public ::testing::Test
{
   protected:
    void SetUp() override
    {
        logger::Logger::instance().set_level(logger::Level::DEBUG);

        BencodeParser parser(TORRENT_PATH);
        torrent = parser.parse();
        ASSERT_NE(torrent, nullptr) << "Failed to parse torrent file";

        tracker = torrent->get_tracker();
        ASSERT_EQ(tracker.protocol, TrackerProtocol::UDP)
            << "Expected a UDP tracker, got: " << tracker.protocol.scheme;

        peer_id = utils::generate_peer_id();

        std::cout << "[  INFO  ] Tracker: " << tracker.to_string() << std::endl;
        std::cout << "[  INFO  ] Info hash: " << torrent->get_info_hash_hex()
                  << std::endl;
        std::cout << "[  INFO  ] Peer ID: " << utils::to_hex(peer_id)
                  << std::endl;
    }

    std::vector<Peer> do_announce(uint64_t downloaded, uint64_t left,
                                  uint64_t uploaded, uint32_t event)
    {
        return communicator.announce(tracker, torrent->get_info_hash(), peer_id,
                                     downloaded, left, uploaded, event,
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
    UDPTrackerCommunicator communicator;
};

TEST_F(UDPTrackerTest, AnnounceStarted)
{
    auto peers = do_announce(0, 0, 0, EVENT_STARTED);
    print_peers(peers);
    EXPECT_GT(peers.size(), 0u) << "Expected at least one peer from started";
}

TEST_F(UDPTrackerTest, AnnounceNone)
{
    auto peers = do_announce(1024, 1000000, 512, EVENT_NONE);
    print_peers(peers);
    EXPECT_GT(peers.size(), 0u) << "Expected at least one peer from re-announce";
}

TEST_F(UDPTrackerTest, AnnounceCompleted)
{
    uint64_t total_size = torrent->get_total_size();
    auto peers = do_announce(total_size, 0, total_size / 2, EVENT_COMPLETED);
    print_peers(peers);
    EXPECT_GT(peers.size(), 0u)
        << "Expected at least one peer from completed announce";
}

TEST_F(UDPTrackerTest, AnnounceStopped)
{
    auto peers = do_announce(0, 0, 0, EVENT_STOPPED);
    print_peers(peers);
    // Stopped may return an empty list on some trackers since they remove us
}

TEST_F(UDPTrackerTest, FullLifecycle)
{
    uint64_t total_size = torrent->get_total_size();

    std::cout << "\n--- Phase 1: started ---" << std::endl;
    auto peers = do_announce(0, total_size, 0, EVENT_STARTED);
    print_peers(peers);
    EXPECT_GT(peers.size(), 0u) << "started should return peers";

    std::cout << "\n--- Phase 2: none (mid-download) ---" << std::endl;
    peers = do_announce(total_size / 4, total_size * 3 / 4, total_size / 8,
                        EVENT_NONE);
    print_peers(peers);
    EXPECT_GT(peers.size(), 0u) << "re-announce should return peers";

    std::cout << "\n--- Phase 3: completed ---" << std::endl;
    peers = do_announce(total_size, 0, total_size / 2, EVENT_COMPLETED);
    print_peers(peers);
    EXPECT_GT(peers.size(), 0u) << "completed should return peers";

    std::cout << "\n--- Phase 4: stopped ---" << std::endl;
    peers = do_announce(total_size, 0, total_size / 2, EVENT_STOPPED);
    print_peers(peers);
}
