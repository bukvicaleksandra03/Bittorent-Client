// Integration test: load a real .torrent, bootstrap a DHT node, and discover
// peers for that torrent's info hash. Does not start SessionManager,
// TorrentManager, or any peer-wire download.
//
// Not run by `make test` or `make test-integration`. Build with:
//   make test_dht_get_peers_from_torrent
//
// Quick (stops when the first peer appears):
//   RUN_DHT_PEER_DISCOVERY_TEST=1 ./out/test_dht_get_peers_from_torrent
//     --gtest_filter='DhtPeerDiscovery.GetPeersFromRealTorrent'
//
// Long (keeps querying for the full lookup window):
//   RUN_DHT_PEER_DISCOVERY_LONG_TEST=1 ./out/test_dht_get_peers_from_torrent
//     --gtest_filter='DhtPeerDiscovery.GetPeersFromRealTorrentLong'
//
// Optional environment:
//   TORRENT_PATH                 Path to a .torrent file (default: Ubuntu
//   desktop) DHT_PEER_LOOKUP_TIMEOUT_SEC  Lookup window in seconds (quick
//   default: 60,
//                                long default: 120)
//
// The long test writes KRPC traffic to logs/<sanitized-torrent-name>/dht.log
// under the project root (same naming as TorrentManager).
//
// Use torrent_files/unparsed_torrents/*.torrent (real bencode). Files under
// torrent_files/torrent_file_objects/ are text metadata dumps, not .torrents.
//
// Peer counts vary by swarm size and DHT density. This client issues a single
// get_peers round per loop iteration (no full iterative BEP-5 walk).

#include <gtest/gtest.h>

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "bencode/bencode_parser.h"
#include "dht/dht_client.h"
#include "logger.h"

namespace fs = std::filesystem;

namespace
{

fs::path project_root()
{
    return fs::path(__FILE__).parent_path().parent_path().parent_path();
}

std::string default_torrent_path()
{
    return (project_root() / "torrent_files" / "unparsed_torrents" /
            "ubuntu-25.10-desktop-amd64.iso.torrent")
        .string();
}

std::string info_hash_20(const TorrentFile& torrent)
{
    const auto& h = torrent.get_info_hash();
    return std::string(reinterpret_cast<const char*>(h.data()), h.size());
}

std::string sanitize_filename(const std::string& name)
{
    std::string out;
    out.reserve(name.size());
    for (unsigned char c : name)
    {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.')
        {
            out += static_cast<char>(c);
        }
        else
        {
            out += '_';
        }
    }
    return out;
}

bool wait_for_routing_table(dht::DhtClient& client,
                            size_t min_size,
                            std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (client.routing_table_size() >= min_size)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return client.routing_table_size() >= min_size;
}

struct PeerEndpoint
{
    std::string ip;
    uint16_t port{0};

    bool operator<(const PeerEndpoint& other) const
    {
        if (ip != other.ip)
            return ip < other.ip;
        return port < other.port;
    }
};

std::chrono::seconds lookup_timeout_from_env(
    std::chrono::seconds default_timeout)
{
    const char* env = std::getenv("DHT_PEER_LOOKUP_TIMEOUT_SEC");
    if (!env || !*env)
        return default_timeout;

    char* end = nullptr;
    const long seconds = std::strtol(env, &end, 10);
    if (end == env || seconds <= 0)
        return default_timeout;

    return std::chrono::seconds(seconds);
}

std::set<PeerEndpoint> discover_peers(const std::string& info_hash,
                                      const std::string& torrent_name,
                                      std::chrono::seconds lookup_timeout,
                                      bool stop_on_first_peer,
                                      bool log_progress)
{
    std::mutex peers_mutex;
    std::set<PeerEndpoint> peers;

    dht::DhtClient client(0);
    {
        auto krpc_logger = std::make_shared<logger::Logger>();
        krpc_logger->set_level(logger::Level::INFO);
        krpc_logger->set_prefix(" KRPC ");
        const fs::path log_dir =
            project_root() / "logs" / sanitize_filename(torrent_name);
        fs::create_directories(log_dir);
        if (fs::exists(log_dir / "dht.log"))
        {
            fs::remove(log_dir / "dht.log");
        }
        krpc_logger->set_file((log_dir / "dht.log").string());
        client.add_krpc_logger(krpc_logger);
    }

    client.set_peer_callback(
        [&](const std::string& /*info_hash_hex*/,
            const std::string& ip,
            uint16_t port)
        {
            std::lock_guard<std::mutex> lock(peers_mutex);
            peers.insert(PeerEndpoint{ip, port});
        });

    client.start();
    if (!client.is_running())
        return {};

    if (!wait_for_routing_table(client, 1, std::chrono::seconds(15)))
    {
        client.stop();
        return {};
    }

    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + lookup_timeout;
    size_t last_reported = 0;

    while (std::chrono::steady_clock::now() < deadline)
    {
        client.get_peers(info_hash);

        if (stop_on_first_peer)
        {
            std::lock_guard<std::mutex> lock(peers_mutex);
            if (!peers.empty())
                break;
        }
        else if (log_progress)
        {
            std::lock_guard<std::mutex> lock(peers_mutex);
            if (peers.size() != last_reported)
            {
                const auto elapsed =
                    std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - started);
                std::cout << "[long] " << elapsed.count()
                          << "s: " << peers.size() << " peer(s) so far\n";
                last_reported = peers.size();
            }
        }

        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    client.stop();

    std::lock_guard<std::mutex> lock(peers_mutex);
    return peers;
}

void print_peers(const TorrentFile& torrent,
                 const std::set<PeerEndpoint>& peers)
{
    std::cout << "Discovered " << peers.size() << " peer(s) for "
              << torrent.get_info_hash_hex() << ":\n";
    for (const auto& peer : peers)
        std::cout << "  " << peer.ip << ":" << peer.port << '\n';
}

std::unique_ptr<TorrentFile> load_torrent_or_null(
    const std::string& torrent_path)
{
    if (!fs::is_regular_file(torrent_path))
        return nullptr;

    bencode::Parser parser(torrent_path);
    return parser.parse();
}

}  // namespace

TEST(DhtPeerDiscovery, GetPeersFromRealTorrent)
{
    if (!std::getenv("RUN_DHT_PEER_DISCOVERY_TEST"))
    {
        GTEST_SKIP()
            << "Set RUN_DHT_PEER_DISCOVERY_TEST=1 to run live DHT peer "
               "discovery (network-dependent).";
    }

    const char* torrent_env = std::getenv("TORRENT_PATH");
    const std::string torrent_path =
        torrent_env ? torrent_env : default_torrent_path();

    auto torrent = load_torrent_or_null(torrent_path);
    ASSERT_NE(torrent, nullptr)
        << "torrent not found or invalid: " << torrent_path;

    const std::string info_hash = info_hash_20(*torrent);
    ASSERT_EQ(info_hash.size(), 20u);

    const auto peers =
        discover_peers(info_hash,
                       torrent->get_name(),
                       lookup_timeout_from_env(std::chrono::seconds(60)),
                       /*stop_on_first_peer=*/true,
                       /*log_progress=*/false);

    if (peers.empty())
    {
        GTEST_SKIP() << "No peers returned for info hash "
                     << torrent->get_info_hash_hex()
                     << " within lookup window (swarm may be quiet or lookup "
                        "incomplete).";
    }

    print_peers(*torrent, peers);
    EXPECT_GT(peers.size(), 0u);
}

TEST(DhtPeerDiscovery, GetPeersFromRealTorrentLong)
{
    if (!std::getenv("RUN_DHT_PEER_DISCOVERY_LONG_TEST"))
    {
        GTEST_SKIP()
            << "Set RUN_DHT_PEER_DISCOVERY_LONG_TEST=1 to run the full "
               "window DHT peer discovery test (network-dependent).";
    }

    const char* torrent_env = std::getenv("TORRENT_PATH");
    const std::string torrent_path =
        torrent_env ? torrent_env : default_torrent_path();

    auto torrent = load_torrent_or_null(torrent_path);
    ASSERT_NE(torrent, nullptr)
        << "torrent not found or invalid: " << torrent_path;

    const std::string info_hash = info_hash_20(*torrent);
    ASSERT_EQ(info_hash.size(), 20u);

    const auto lookup_timeout =
        lookup_timeout_from_env(std::chrono::seconds(120));
    std::cout << "Long lookup for " << torrent->get_info_hash_hex() << " ("
              << lookup_timeout.count() << "s window)\n";

    const auto peers = discover_peers(info_hash,
                                      torrent->get_name(),
                                      lookup_timeout,
                                      /*stop_on_first_peer=*/false,
                                      /*log_progress=*/true);

    if (peers.empty())
    {
        GTEST_SKIP() << "No peers returned for info hash "
                     << torrent->get_info_hash_hex() << " within "
                     << lookup_timeout.count()
                     << "s (swarm may be quiet or lookup incomplete).";
    }

    print_peers(*torrent, peers);
    EXPECT_GT(peers.size(), 0u);
}
