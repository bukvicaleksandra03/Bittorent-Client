// tests/dht/test_dht_peer_store.cpp
// Direct DhtPeerStore unit tests (global cap + LRU eviction at
// MAX_INFO_HASHES).

#include <gtest/gtest.h>

#include <string>

#include "dht/dht_peer_store.h"
#include "info_hash.h"

namespace
{

InfoHash make_info_hash(size_t n)
{
    InfoHash key;
    key.bytes[0] = static_cast<uint8_t>((n >> 24) & 0xFF);
    key.bytes[1] = static_cast<uint8_t>((n >> 16) & 0xFF);
    key.bytes[2] = static_cast<uint8_t>((n >> 8) & 0xFF);
    key.bytes[3] = static_cast<uint8_t>(n & 0xFF);
    return key;
}

}  // namespace

TEST(DhtPeerStore, EvictsLeastRecentlyUsedInfoHashWhenAtCapacity)
{
    dht::DhtPeerStore store;
    const PeerAddress peer("127.0.0.1", 6881);

    for (size_t i = 0; i < 10000; ++i)
        store.upsert(make_info_hash(i), peer);

    // Refresh hash 0 so hash 1 becomes the LRU bucket when we insert hash
    // 10000.
    store.upsert(make_info_hash(0), peer);
    store.upsert(make_info_hash(10000), peer);

    EXPECT_FALSE(store.live_peers(make_info_hash(0)).empty())
        << "refreshed hash 0 should remain";
    EXPECT_TRUE(store.live_peers(make_info_hash(1)).empty())
        << "hash 1 should have been evicted as LRU";
    EXPECT_FALSE(store.live_peers(make_info_hash(10000)).empty())
        << "new hash 10000 should be present after eviction";
}
