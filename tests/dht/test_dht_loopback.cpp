// tests/dht/test_dht_loopback.cpp
// DHT tests that exercise real UDP I/O.
//
// LocalPingPopulatesRoutingTable — localhost only, no public routers required.
// BootstrapPopulatesTable — hits public bootstrap nodes (network-dependent).

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>

#include "dht/dht_client.h"
#include "dht/krpc.h"
#include "dht/node_id.h"
#include "info_hash.h"
#include "net/socket.h"
#include "net/socket_addresses.h"
#include "peer_address.h"

namespace
{

int next_loopback_port()
{
    static std::atomic<int> port_counter{55200};
    return ++port_counter;
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

std::string info_hash_hex_from_byte(char byte)
{
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(2)
        << static_cast<unsigned>(static_cast<unsigned char>(byte));
    return oss.str();
}

std::string repeated_info_hash_hex(char byte)
{
    const std::string pair = info_hash_hex_from_byte(byte);
    std::string hex;
    hex.reserve(40);
    for (int i = 0; i < 20; ++i)
        hex += pair;
    return hex;
}

InfoHash info_hash_from_byte(char byte)
{
    InfoHash h;
    h.bytes.fill(static_cast<uint8_t>(byte));
    return h;
}

bool seed_peer_on_server(uint16_t server_port,
                         const InfoHash& info_hash,
                         const std::string& peer_ip,
                         uint16_t peer_port)
{
    (void)peer_ip;  // announce_peer records the UDP source address
    dht::NodeId announcer_id = dht::NodeId::random();
    UDPSocket sock(AF_INET);
    sock.bind(IPv4Address("0.0.0.0", 0));

    const std::string txn = dht::random_txn();
    sock.sendto(dht::make_get_peers(txn, announcer_id, info_hash),
                IPv4Address("127.0.0.1", server_port));

    std::string token;
    try
    {
        auto [data, addr] = sock.recvfrom_with_timeout(2000);
        (void)addr;
        auto parsed = dht::parse_krpc(data);
        if (!parsed || parsed->token.empty())
            return false;
        token = parsed->token;
    }
    catch (const std::exception&)
    {
        return false;
    }

    sock.sendto(dht::make_announce_peer(dht::random_txn(),
                                        announcer_id,
                                        info_hash,
                                        peer_port,
                                        token,
                                        false),
                IPv4Address("127.0.0.1", server_port));
    return true;
}

}  // namespace

TEST(DhtClient, LocalPingPopulatesRoutingTable)
{
    const uint16_t port = static_cast<uint16_t>(next_loopback_port());

    dht::DhtClient server(port);
    server.start();
    ASSERT_TRUE(server.is_running());

    dht::NodeId client_id = dht::NodeId::random();
    UDPSocket client(AF_INET);
    IPv4Address dest("127.0.0.1", port);
    client.sendto(dht::make_ping(dht::random_txn(), client_id), dest);

    EXPECT_TRUE(wait_for_routing_table(server, 1, std::chrono::seconds(2)))
        << "expected ping sender in routing table";

    server.stop();
}

TEST(DhtClient, GetPeersRoutesByTransactionId)
{
    const uint16_t server_port = static_cast<uint16_t>(next_loopback_port());
    const uint16_t peer_port = static_cast<uint16_t>(next_loopback_port());

    const InfoHash info_hash = info_hash_from_byte('\x42');
    const std::string expected_hex = repeated_info_hash_hex('\x42');

    dht::DhtClient server(server_port);
    server.start();
    ASSERT_TRUE(server.is_running());

    ASSERT_TRUE(seed_peer_on_server(
        server_port, info_hash, "127.0.0.1", peer_port));

    dht::DhtClient lookup_client(0);
    lookup_client.set_bootstrap_on_start(false);
    lookup_client.start();
    ASSERT_TRUE(lookup_client.is_running());

    lookup_client.ping(PeerAddress("127.0.0.1", server_port));
    ASSERT_TRUE(wait_for_routing_table(
        lookup_client, 1, std::chrono::seconds(2)));

    std::mutex mu;
    std::condition_variable cv;
    std::string cb_hash;
    std::string cb_ip;
    uint16_t cb_port = 0;
    bool got_peer = false;

    lookup_client.set_peer_callback(
        [&](const std::string& hash_hex, PeerAddress pa)
        {
            std::lock_guard<std::mutex> lk(mu);
            cb_hash = hash_hex;
            cb_ip = pa.ip;
            cb_port = pa.port;
            got_peer = true;
            cv.notify_all();
        });

    lookup_client.get_peers(info_hash);

    {
        std::unique_lock<std::mutex> lk(mu);
        EXPECT_TRUE(cv.wait_for(
            lk, std::chrono::seconds(2), [&] { return got_peer; }));
    }

    EXPECT_EQ(cb_hash, expected_hex);
    EXPECT_EQ(cb_ip, "127.0.0.1");
    EXPECT_EQ(cb_port, peer_port);

    lookup_client.stop();
    server.stop();
}

TEST(DhtClient, GetPeersSeparatesMultipleInfoHashes)
{
    const uint16_t server_port = static_cast<uint16_t>(next_loopback_port());
    const uint16_t peer_port_a = static_cast<uint16_t>(next_loopback_port());
    const uint16_t peer_port_b = static_cast<uint16_t>(next_loopback_port());

    const InfoHash hash_a = info_hash_from_byte('\x11');
    const InfoHash hash_b = info_hash_from_byte('\x22');
    const std::string hex_a = repeated_info_hash_hex('\x11');
    const std::string hex_b = repeated_info_hash_hex('\x22');

    dht::DhtClient server(server_port);
    server.start();

    ASSERT_TRUE(seed_peer_on_server(
        server_port, hash_a, "127.0.0.1", peer_port_a));
    ASSERT_TRUE(seed_peer_on_server(
        server_port, hash_b, "127.0.0.1", peer_port_b));

    dht::DhtClient lookup_client(0);
    lookup_client.set_bootstrap_on_start(false);
    lookup_client.start();
    lookup_client.ping(PeerAddress("127.0.0.1", server_port));
    ASSERT_TRUE(wait_for_routing_table(
        lookup_client, 1, std::chrono::seconds(2)));

    std::mutex mu;
    std::condition_variable cv;
    std::unordered_map<std::string, uint16_t> seen_ports;
    int callback_count = 0;

    lookup_client.set_peer_callback(
        [&](const std::string& hash_hex, PeerAddress pa)
        {
            if (pa.ip != "127.0.0.1")
                return;
            std::lock_guard<std::mutex> lk(mu);
            seen_ports[hash_hex] = pa.port;
            ++callback_count;
            cv.notify_all();
        });

    lookup_client.get_peers(hash_a);
    lookup_client.get_peers(hash_b);

    {
        std::unique_lock<std::mutex> lk(mu);
        EXPECT_TRUE(cv.wait_for(
            lk,
            std::chrono::seconds(2),
            [&] { return callback_count >= 2; }));
    }

    EXPECT_EQ(seen_ports[hex_a], peer_port_a);
    EXPECT_EQ(seen_ports[hex_b], peer_port_b);

    lookup_client.stop();
    server.stop();
}

TEST(DhtClient, BootstrapPopulatesTable)
{
    dht::DhtClient client(0);
    client.start();
    const bool populated =
        wait_for_routing_table(client, 1, std::chrono::seconds(8));
    if (!populated)
    {
        client.stop();
        GTEST_SKIP() << "public DHT bootstrap unreachable (network/VPN/firewall)";
    }
    EXPECT_GT(client.routing_table_size(), 0u);
    client.stop();
}
