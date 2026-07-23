// tests/dht/test_dht_peer_store.cpp
// Direct DhtPeerStore unit tests (global cap + LRU eviction at
// MAX_INFO_HASHES).

#include <gtest/gtest.h>

#include <string>

#include "dht/dht_peer_store.h"

namespace
{

std::string make_info_hash(size_t n)
{
    std::string key(20, '\0');
    key[0] = static_cast<char>((n >> 24) & 0xFF);
    key[1] = static_cast<char>((n >> 16) & 0xFF);
    key[2] = static_cast<char>((n >> 8) & 0xFF);
    key[3] = static_cast<char>(n & 0xFF);
    return key;
}

}  // namespace

TEST(DhtPeerStore, EvictsLeastRecentlyUsedInfoHashWhenAtCapacity)
{
    dht::DhtPeerStore store;
    const std::string compact(6, '\x01');

    for (size_t i = 0; i < 10000; ++i)
        store.upsert(make_info_hash(i), compact);

    // Refresh hash 0 so hash 1 becomes the LRU bucket when we insert hash
    // 10000.
    store.upsert(make_info_hash(0), compact);
    store.upsert(make_info_hash(10000), compact);

    EXPECT_FALSE(store.live_peers(make_info_hash(0)).empty())
        << "refreshed hash 0 should remain";
    EXPECT_TRUE(store.live_peers(make_info_hash(1)).empty())
        << "hash 1 should have been evicted as LRU";
    EXPECT_FALSE(store.live_peers(make_info_hash(10000)).empty())
        << "new hash 10000 should be present after eviction";
}
