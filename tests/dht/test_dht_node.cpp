// tests/dht/test_dht_node.cpp
// Unit tests for dht::DhtNode – lifecycle only (no real network activity)

#include <gtest/gtest.h>

#include "dht/dht_node.h"

#include <chrono>
#include <thread>

// ===========================================================================
// DhtNode – lifecycle only (no real network activity)
// ===========================================================================

TEST(DhtNode, StartStop)
{
    // Use an ephemeral port (0) so we don't clash with anything.
    dht::DhtNode node(0);
    EXPECT_FALSE(node.is_running());

    node.start();
    EXPECT_TRUE(node.is_running());

    node.stop();
    EXPECT_FALSE(node.is_running());
}

TEST(DhtNode, DoubleStop)
{
    dht::DhtNode node(0);
    node.start();
    node.stop();
    // Second stop must not crash.
    EXPECT_NO_THROW(node.stop());
}

TEST(DhtNode, SelfIdUnique)
{
    dht::DhtNode a(0), b(0);
    EXPECT_NE(a.self_id().to_string(), b.self_id().to_string());
}

TEST(DhtNode, RoutingTableInitiallyEmpty)
{
    dht::DhtNode node(0);
    EXPECT_EQ(node.routing_table_size(), 0u);
}

TEST(DhtNode, PeerCallbackSet)
{
    // Just verify we can set a callback without crashing.
    dht::DhtNode node(0);
    bool called = false;
    node.set_peer_callback(
        [&called](const std::string&, const std::string&, uint16_t) {
            called = true;
        });
    node.start();
    // Give bootstrap a very short window; we don't expect real peers in a unit
    // test, so `called` remains false — we only check no crash occurs.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    node.stop();
    // (called might be false; that's fine — no assertion on it)
    (void)called;
}
