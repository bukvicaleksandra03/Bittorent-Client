// tests/dht/test_routing_table.cpp
// Unit tests for dht::RoutingTable and dht::RoutingEntry

#include <gtest/gtest.h>

#include "dht/node_id.h"
#include "dht/routing_table.h"
#include "peer_address.h"

#include <string>

// ===========================================================================
// RoutingTable
// ===========================================================================

TEST(RoutingTable, InsertAndFind)
{
    dht::NodeId self = dht::NodeId::random();
    dht::RoutingTable rt(self);

    dht::NodeId nid = dht::NodeId::random();
    dht::RoutingEntry entry{nid, PeerAddress("127.0.0.1", 6881)};

    rt.add(entry);
    EXPECT_GE(rt.size(), 1u);

    auto closest = rt.closest(nid, 8);
    ASSERT_FALSE(closest.empty());
    EXPECT_EQ(closest[0].id, nid);
}

TEST(RoutingTable, ClosestOrder)
{
    dht::NodeId self{};   // all-zeros
    dht::RoutingTable rt(self);

    // Insert several nodes at known distances
    for (uint8_t i = 1; i <= 10; ++i)
    {
        dht::NodeId id{};
        id.bytes[19] = i;
        rt.add({id, PeerAddress("127.0.0.1", static_cast<uint16_t>(6880 + i))});
    }

    dht::NodeId target{};
    target.bytes[19] = 3;
    auto closest = rt.closest(target, 3);

    // The closest node should be distance-0 (id == target, i.e. i=3)
    ASSERT_FALSE(closest.empty());
    EXPECT_EQ(closest[0].id.bytes[19], 3u);
}

TEST(RoutingTable, MaxSize)
{
    dht::NodeId self{};
    dht::RoutingTable rt(self);

    // Fill more than default max_size nodes
    for (int i = 0; i < 20; ++i)
    {
        dht::NodeId id = dht::NodeId::random();
        rt.add({id, PeerAddress("127.0.0.1", static_cast<uint16_t>(7000 + i))});
    }
    // Table should not grow unboundedly (NUM_BUCKETS * K = 1280)
    EXPECT_LE(rt.size(), dht::RoutingTable::NUM_BUCKETS * dht::RoutingTable::K);
}

TEST(RoutingTable, GetAllNodes)
{
    dht::NodeId self = dht::NodeId::random();
    dht::RoutingTable rt(self);

    for (int i = 0; i < 5; ++i)
    {
        dht::NodeId id = dht::NodeId::random();
        rt.add({id, PeerAddress("10.0.0." + std::to_string(i + 1), 6881)});
    }

    auto all = rt.all();
    EXPECT_EQ(all.size(), 5u);
}
