#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <unistd.h>
#include <vector>

#include "dht/node_id.h"
#include "dht/routing_table.h"
#include "dht/routing_table_store.h"
#include "peer_address.h"

namespace
{

std::string temp_path()
{
    return "/tmp/dht_routing_table_test_" + std::to_string(getpid()) + ".txt";
}

dht::RoutingEntry make_entry(uint8_t last_byte, uint16_t port)
{
    dht::NodeId id{};
    id.bytes[19] = last_byte;
    dht::RoutingEntry entry;
    entry.id = id;
    entry.pa = PeerAddress("127.0.0.1", port);
    entry.last_seen = std::chrono::steady_clock::now();
    return entry;
}

}  // namespace

TEST(RoutingTableStore, SaveLoadRoundtrip)
{
    const std::string path = temp_path();
    std::remove(path.c_str());

    dht::NodeId self = dht::NodeId::random();
    dht::RoutingTable table(self);

    for (uint8_t i = 1; i <= 6; ++i)
        table.add(make_entry(i, static_cast<uint16_t>(6880 + i)));

    std::string err;
    ASSERT_TRUE(dht::RoutingTableStore::save(table, self, path, &err)) << err;

    const auto loaded = dht::RoutingTableStore::load(path);
    ASSERT_TRUE(loaded.ok) << loaded.error;
    EXPECT_EQ(loaded.snapshot.entries.size(), 6u);
    EXPECT_EQ(loaded.snapshot.entries[0].id.bytes[19], 1);

    std::remove(path.c_str());
}

TEST(RoutingTableStore, SkipBootstrapThresholdRequiresVerifiedGood)
{
    std::vector<dht::RoutingEntry> entries;
    for (uint8_t i = 1; i <= 4; ++i)
        entries.push_back(make_entry(i, static_cast<uint16_t>(6880 + i)));

    size_t good = 0;
    for (const auto& e : entries)
    {
        if (e.is_good())
            ++good;
    }
    EXPECT_GE(good, dht::kMinGoodEntriesToSkipBootstrap);
}

TEST(RoutingTableStore, LoadedEntriesNotGoodUntilContact)
{
    const std::string path = temp_path();
    std::remove(path.c_str());

    dht::NodeId self = dht::NodeId::random();
    dht::RoutingTable table(self);

    for (uint8_t i = 1; i <= 6; ++i)
        table.add(make_entry(i, static_cast<uint16_t>(6880 + i)));

    std::string err;
    ASSERT_TRUE(dht::RoutingTableStore::save(table, self, path, &err)) << err;

    const auto loaded = dht::RoutingTableStore::load(path);
    ASSERT_TRUE(loaded.ok) << loaded.error;

    for (const auto& e : loaded.snapshot.entries)
        EXPECT_FALSE(e.is_good());

    std::remove(path.c_str());
}

TEST(RoutingTableStore, RemoveMissingFileSucceeds)
{
    const std::string path = temp_path();
    std::remove(path.c_str());
    std::string err;
    EXPECT_TRUE(dht::RoutingTableStore::remove_file(path, &err)) << err;
}
