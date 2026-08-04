#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#include "info_hash.h"
#include "lru_cache.h"
#include "peer_address.h"

namespace dht
{

// Maps info_hash -> LRU cache of compact peer entries (ip:port) to their
// last-seen time. Populated when:
//   - We handle an announce_peer query as a server (on_announce_peer) --
//     someone tells us they're seeding/leeching a torrent.
//   - We receive peers in a get_peers response while running our own lookup
//     (on_response).
// Entries expire after 30 minutes (PEER_ENTRY_TTL). Both the peers per hash
// (50) and the hashes overall (10,000) are capped by utils::LruCache, giving
// O(1) eviction at either level.
class DhtPeerStore
{
   public:
    DhtPeerStore() : buckets_(MAX_INFO_HASHES) {}

    // Record or refresh a compact peer (6-byte ip:port) for info_hash;
    // creates the bucket if needed, evicts LRU info-hash at global cap,
    // and evicts the LRU peer when the per-hash cache is full.
    void upsert(const InfoHash& info_hash, PeerAddress pa);

    // Non-expired compact peer strings for info_hash; refreshes bucket LRU.
    std::vector<PeerAddress> live_peers(const InfoHash& info_hash);

    // Create or touch a bucket (used when starting a get_peers lookup).
    void ensure_bucket(const InfoHash& info_hash);

    // Drop hashes whose most-recently-seen peer is past PEER_ENTRY_TTL (i.e.
    // hashes with no live peers left).
    void expire_stale();

   private:
    static constexpr auto PEER_ENTRY_TTL = std::chrono::minutes(30);
    static constexpr size_t MAX_PEERS_PER_HASH = 50;
    static constexpr size_t MAX_INFO_HASHES = 10000;

    struct HashBucket
    {
        HashBucket() : peers(MAX_PEERS_PER_HASH) {}

        // Peer Addres -> last-seen time, LRU-evicted once the
        // bucket holds MAX_PEERS_PER_HASH peers.
        utils::LruCache<PeerAddress, std::chrono::steady_clock::time_point>
            peers;
    };

    utils::LruCache<InfoHash, HashBucket> buckets_;
    mutable std::mutex mutex_;
};

}  // namespace dht
