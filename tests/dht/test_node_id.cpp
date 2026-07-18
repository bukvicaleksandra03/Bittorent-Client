// tests/dht/test_node_id.cpp
// Unit tests for dht::NodeId

#include <gtest/gtest.h>

#include "dht/node_id.h"

#include <set>
#include <string>

// ===========================================================================
// NodeId
// ===========================================================================

TEST(NodeId, ZeroCheck)
{
    dht::NodeId zero{};
    EXPECT_TRUE(zero.is_zero());

    dht::NodeId nonzero = dht::NodeId::random();
    // Statistically impossible to be zero
    EXPECT_FALSE(nonzero.is_zero());
}

TEST(NodeId, UniqueRandom)
{
    std::set<std::string> seen;
    for (int i = 0; i < 100; ++i)
        seen.insert(dht::NodeId::random().to_string());
    EXPECT_EQ(seen.size(), 100u);
}

TEST(NodeId, XorDistance)
{
    dht::NodeId a{}, b{};
    a.bytes[0] = 0x0F;
    b.bytes[0] = 0x03;

    dht::NodeId dist = a ^ b;
    EXPECT_EQ(dist.bytes[0], 0x0Cu);
    for (size_t i = 1; i < 20; ++i)
        EXPECT_EQ(dist.bytes[i], 0u);
}

TEST(NodeId, XorSelf)
{
    dht::NodeId id = dht::NodeId::random();
    EXPECT_TRUE((id ^ id).is_zero());
}

TEST(NodeId, Comparison)
{
    dht::NodeId a{}, b{};
    a.bytes[0] = 0x01;
    b.bytes[0] = 0x02;
    EXPECT_TRUE(a < b);
    EXPECT_FALSE(b < a);
    EXPECT_TRUE(a == a);
    EXPECT_TRUE(a != b);
}

TEST(NodeId, HexLength)
{
    std::string h = dht::NodeId::random().hex();
    EXPECT_EQ(h.size(), 40u);
    for (char c : h)
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
}

TEST(NodeId, FromString)
{
    std::string s = "abcdefghij0123456789";   // exactly 20 bytes
    dht::NodeId id = dht::NodeId::from_string(s);
    EXPECT_EQ(id.to_string(), s);
}
