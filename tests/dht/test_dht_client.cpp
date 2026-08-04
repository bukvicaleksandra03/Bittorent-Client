// tests/dht/test_dht_client.cpp
// Unit tests for dht::DhtClient – lifecycle only (no real network activity)

#include <gtest/gtest.h>

#include "dht/dht_client.h"
#include "info_hash.h"

#include <chrono>
#include <thread>

namespace
{

InfoHash make_info_hash(char fill = '\xAB')
{
    InfoHash h;
    h.bytes.fill(static_cast<uint8_t>(fill));
    return h;
}

}  // namespace

// ===========================================================================
// DhtClient – lifecycle only (no real network activity)
// ===========================================================================

TEST(DhtClient, StartStop)
{
    // Use an ephemeral port (0) so we don't clash with anything.
    dht::DhtClient client(0);
    EXPECT_FALSE(client.is_running());

    client.start();
    EXPECT_TRUE(client.is_running());

    client.stop();
    EXPECT_FALSE(client.is_running());
}

TEST(DhtClient, DoubleStop)
{
    dht::DhtClient client(0);
    client.start();
    client.stop();
    // Second stop must not crash.
    EXPECT_NO_THROW(client.stop());
}

TEST(DhtClient, SelfIdUnique)
{
    dht::DhtClient a(0), b(0);
    EXPECT_NE(a.self_id().to_string(), b.self_id().to_string());
}

TEST(DhtClient, RoutingTableInitiallyEmpty)
{
    dht::DhtClient client(0);
    EXPECT_EQ(client.routing_table_size(), 0u);
}

TEST(DhtClient, RegisterUnregisterTorrent)
{
    dht::DhtClient client(0);
    const InfoHash info_hash = make_info_hash();

    EXPECT_NO_THROW(client.register_torrent(info_hash, 6881));
    EXPECT_NO_THROW(client.register_torrent(InfoHash{}, 6881));
    EXPECT_NO_THROW(client.unregister_torrent(info_hash));

    client.start();
    EXPECT_NO_THROW(client.register_torrent(info_hash, 6881));
    EXPECT_NO_THROW(client.unregister_torrent(info_hash));
    client.stop();
}

TEST(DhtClient, PeerCallbackSet)
{
    // Just verify we can set a callback without crashing.
    dht::DhtClient client(0);
    bool called = false;
    client.set_peer_callback(
        [&called](const std::string&, PeerAddress) { called = true; });
    client.start();
    // Give bootstrap a very short window; we don't expect real peers in a unit
    // test, so `called` remains false — we only check no crash occurs.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    client.stop();
    // (called might be false; that's fine — no assertion on it)
    (void)called;
}
