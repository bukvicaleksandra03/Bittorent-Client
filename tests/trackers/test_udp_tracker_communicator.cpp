#include <gtest/gtest.h>

#include <iostream>
#include <memory>
#include <string>

#include "bencode/bencode_parser.h"
#include "peer.h"
#include "torrent_file.h"
#include "trackers/udp_tracker_communicator.h"
#include "utils.h"

static const std::string TORRENT_PATH =
    "torrent_files/unparsed_torrents/linuxmint-22.2-cinnamon-64bit.iso.torrent";

TEST(UDPTrackerCommunicator, AnnounceStartedReturnsPeers)
{
    BencodeParser parser(TORRENT_PATH);
    auto torrent = parser.parse();
    ASSERT_NE(torrent, nullptr) << "Failed to parse torrent file";

    TrackerDetails tracker = torrent->get_tracker();
    ASSERT_EQ(tracker.protocol, TrackerProtocol::UDP)
        << "Expected a UDP tracker, got: " << tracker.protocol.scheme;

    std::cout << "[  INFO  ] Tracker: " << tracker.to_string() << std::endl;
    std::cout << "[  INFO  ] Info hash: " << torrent->get_info_hash_hex()
              << std::endl;

    utils::PeerId my_peer_id = utils::generate_peer_id();
    std::cout << "[  INFO  ] Peer ID: " << utils::to_hex(my_peer_id) << std::endl;

    UDPTrackerCommunicator communicator;

    constexpr uint32_t EVENT_STARTED = 2;
    constexpr uint16_t LISTEN_PORT = 6881;

    std::vector<Peer> peers;
    ASSERT_NO_THROW({
        peers = communicator.announce(tracker,
                                      torrent->get_info_hash(),
                                      my_peer_id,
                                      0,
                                      0,
                                      0,
                                      EVENT_STARTED,
                                      LISTEN_PORT);
    }) << "announce() threw an exception";

    std::cout << "[  INFO  ] Received " << peers.size() << " peers" << std::endl;
    EXPECT_GT(peers.size(), 0u) << "Expected at least one peer";

    for (size_t i = 0; i < std::min(peers.size(), size_t(10)); ++i)
    {
        std::cout << "[  INFO  ]   peer " << i << ": " << peers[i].to_string()
                  << std::endl;
    }
}
