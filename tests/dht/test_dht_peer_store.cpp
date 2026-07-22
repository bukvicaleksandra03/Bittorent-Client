// tests/dht/test_dht_peer_store.cpp
// Peer store global cap + LRU eviction (built with -DDHT_MAX_INFO_HASHES=4).

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>

#include "dht/dht_client.h"
#include "dht/krpc.h"
#include "dht/node_id.h"
#include "net/socket.h"
#include "net/socket_addresses.h"

namespace
{

int next_loopback_port()
{
    static std::atomic<int> port_counter{55400};
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

std::string repeated_info_hash_hex(char byte)
{
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(2)
        << static_cast<unsigned>(static_cast<unsigned char>(byte));
    const std::string pair = oss.str();
    std::string hex;
    hex.reserve(40);
    for (int i = 0; i < 20; ++i)
        hex += pair;
    return hex;
}

std::string info_hash_from_byte(char byte)
{
    return std::string(20, byte);
}

bool seed_peer_on_server(uint16_t server_port,
                         const std::string& info_hash_20,
                         const std::string& peer_ip,
                         uint16_t peer_port)
{
    (void)peer_ip;  // announce_peer records the UDP source address
    dht::NodeId announcer_id = dht::NodeId::random();
    UDPSocket sock(AF_INET);
    sock.bind(IPv4Address("0.0.0.0", 0));

    const std::string txn = dht::random_txn();
    sock.sendto(dht::make_get_peers(txn, announcer_id, info_hash_20),
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
                                        info_hash_20,
                                        peer_port,
                                        token,
                                        false),
                IPv4Address("127.0.0.1", server_port));
    return true;
}

bool wait_for_peer(dht::DhtClient& client,
                   const std::string& info_hash_20,
                   const std::string& expected_hex,
                   uint16_t expected_port,
                   std::chrono::milliseconds timeout)
{
    std::mutex mu;
    std::condition_variable cv;
    bool matched = false;

    client.set_peer_callback(
        [&](const std::string& hash_hex,
            const std::string& ip,
            uint16_t port)
        {
            if (hash_hex != expected_hex || ip != "127.0.0.1" ||
                port != expected_port)
            {
                return;
            }
            std::lock_guard<std::mutex> lk(mu);
            matched = true;
            cv.notify_all();
        });

    client.get_peers(info_hash_20);

    std::unique_lock<std::mutex> lk(mu);
    return cv.wait_for(lk, timeout, [&] { return matched; });
}

bool wait_for_no_peer(dht::DhtClient& client,
                      const std::string& info_hash_20,
                      const std::string& expected_hex,
                      std::chrono::milliseconds timeout)
{
    std::mutex mu;
    std::condition_variable cv;
    bool got_wrong_peer = false;

    client.set_peer_callback(
        [&](const std::string& hash_hex,
            const std::string& ip,
            uint16_t port)
        {
            if (hash_hex == expected_hex && ip == "127.0.0.1" && port != 0)
            {
                std::lock_guard<std::mutex> lk(mu);
                got_wrong_peer = true;
                cv.notify_all();
            }
        });

    client.get_peers(info_hash_20);

    std::unique_lock<std::mutex> lk(mu);
    const bool timed_out =
        !cv.wait_for(lk, timeout, [&] { return got_wrong_peer; });
    return timed_out && !got_wrong_peer;
}

}  // namespace

TEST(DhtClient, EvictsLeastRecentlyUsedInfoHashWhenAtCapacity)
{
    const uint16_t server_port = static_cast<uint16_t>(next_loopback_port());

    dht::DhtClient server(server_port);
    server.start();
    ASSERT_TRUE(server.is_running());

    const std::string hash_a = info_hash_from_byte('\x01');
    const std::string hash_b = info_hash_from_byte('\x02');
    const std::string hash_c = info_hash_from_byte('\x03');
    const std::string hash_d = info_hash_from_byte('\x04');
    const std::string hash_e = info_hash_from_byte('\x05');

    const uint16_t port_a = static_cast<uint16_t>(next_loopback_port());
    const uint16_t port_b = static_cast<uint16_t>(next_loopback_port());
    const uint16_t port_c = static_cast<uint16_t>(next_loopback_port());
    const uint16_t port_d = static_cast<uint16_t>(next_loopback_port());
    const uint16_t port_e = static_cast<uint16_t>(next_loopback_port());

    ASSERT_TRUE(seed_peer_on_server(server_port, hash_a, "127.0.0.1", port_a));
    ASSERT_TRUE(seed_peer_on_server(server_port, hash_b, "127.0.0.1", port_b));
    ASSERT_TRUE(seed_peer_on_server(server_port, hash_c, "127.0.0.1", port_c));
    ASSERT_TRUE(seed_peer_on_server(server_port, hash_d, "127.0.0.1", port_d));

    // Refresh hash A so B becomes the LRU bucket when we insert hash E.
    ASSERT_TRUE(seed_peer_on_server(server_port, hash_a, "127.0.0.1", port_a));
    ASSERT_TRUE(seed_peer_on_server(server_port, hash_e, "127.0.0.1", port_e));

    dht::DhtClient lookup_client(0);
    lookup_client.start();
    lookup_client.ping("127.0.0.1", server_port);
    ASSERT_TRUE(wait_for_routing_table(
        lookup_client, 1, std::chrono::seconds(2)));

    EXPECT_TRUE(wait_for_no_peer(
        lookup_client,
        hash_b,
        repeated_info_hash_hex('\x02'),
        std::chrono::milliseconds(1500)))
        << "hash B should have been evicted as LRU";

    EXPECT_TRUE(wait_for_peer(
        lookup_client,
        hash_e,
        repeated_info_hash_hex('\x05'),
        port_e,
        std::chrono::milliseconds(1500)))
        << "hash E should be present after eviction";

    EXPECT_TRUE(wait_for_peer(
        lookup_client,
        hash_a,
        repeated_info_hash_hex('\x01'),
        port_a,
        std::chrono::milliseconds(1500)))
        << "refreshed hash A should remain";

    lookup_client.stop();
    server.stop();
}
