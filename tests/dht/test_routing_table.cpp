// tests/dht/test_routing_table.cpp
// Unit tests for dht::RoutingTable and dht::RoutingEntry

#include <gtest/gtest.h>

#include "dht/node_id.h"
#include "dht/routing_table.h"
#include "peer_address.h"

#include <string>

namespace {

size_t first_bucket_with(const dht::RoutingTable::BucketSnapshot& snap,
                         size_t value)
{
    for (size_t i = 0; i < snap.size(); ++i)
    {
        if (snap[i] == value)
            return i;
    }
    ADD_FAILURE() << "no bucket with value " << value;
    return snap.size();
}

// Build a node ID that maps to bucket when self_id is all zeros.
dht::NodeId id_for_bucket(size_t bucket, unsigned variant)
{
    dht::NodeId id{};
    const size_t byte_idx = bucket / 8;
    const unsigned bit = 7u - static_cast<unsigned>(bucket % 8);
    id.bytes[byte_idx] = static_cast<uint8_t>(1u << bit);
    id.bytes[18] = static_cast<uint8_t>((variant >> 8) & 0xFF);
    id.bytes[19] = static_cast<uint8_t>(variant & 0xFF);
    return id;
}

}  // namespace

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

TEST(RoutingTable, BucketOccupancySnapshot)
{
    dht::NodeId self{};
    dht::RoutingTable rt(self);

    dht::NodeId id{};
    id.bytes[19] = 0x80;
    rt.add({id, PeerAddress("127.0.0.1", 6881)});

    const auto occ = rt.bucket_occupancy();
    const size_t bucket = first_bucket_with(occ, 1u);
    EXPECT_EQ(occ[bucket], 1u);
    EXPECT_EQ(rt.size(), 1u);
}

TEST(RoutingTable, PeersReceivedCountsEveryAdd)
{
    dht::NodeId self{};
    dht::RoutingTable rt(self);

    dht::NodeId id{};
    id.bytes[19] = 0x80;
    dht::RoutingEntry entry{id, PeerAddress("127.0.0.1", 6881)};

    rt.add(entry);
    auto recv = rt.peers_received_per_bucket();
    const size_t bucket = first_bucket_with(recv, 1u);
    EXPECT_EQ(recv[bucket], 1u);

    // MRU update of an existing entry still counts as received.
    rt.add(entry);
    recv = rt.peers_received_per_bucket();
    EXPECT_EQ(recv[bucket], 2u);

    // Persistence restore should not inflate receive counters.
    rt.add(entry, false);
    recv = rt.peers_received_per_bucket();
    EXPECT_EQ(recv[bucket], 2u);
}

TEST(RoutingTable, PeersReceivedWhenBucketFull)
{
    dht::NodeId self{};
    dht::RoutingTable rt(self);
    constexpr size_t target_bucket = 42;

    for (unsigned i = 0; i < dht::RoutingTable::K; ++i)
    {
        dht::NodeId id = id_for_bucket(target_bucket, i);
        EXPECT_EQ(rt.bucket_index(id), target_bucket);
        rt.add({id, PeerAddress("127.0.0.1", static_cast<uint16_t>(7000 + i))});
    }

    EXPECT_EQ(rt.bucket_occupancy()[target_bucket], dht::RoutingTable::K);

    dht::NodeId extra = id_for_bucket(target_bucket, 999);
    EXPECT_EQ(rt.bucket_index(extra), target_bucket);
    rt.add({extra, PeerAddress("127.0.0.1", 7999)});

    const auto recv = rt.peers_received_per_bucket();
    EXPECT_EQ(recv[target_bucket],
              static_cast<size_t>(dht::RoutingTable::K) + 1u);
    EXPECT_EQ(rt.bucket_occupancy()[target_bucket], dht::RoutingTable::K);
}
