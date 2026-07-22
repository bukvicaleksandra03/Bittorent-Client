#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace dht
{

// Per-info-hash peer list with TTL and LRU eviction (BEP 5 announce_peer storage).
class DhtPeerStore
{
   public:
    void upsert(const std::string& info_hash, const std::string& compact);

    // Non-expired compact peer strings for info_hash; refreshes bucket LRU.
    std::vector<std::string> live_peers(const std::string& info_hash);

    // Create or touch a bucket (used when starting a get_peers lookup).
    void ensure_bucket(const std::string& info_hash);

    // Drop peer entries and empty buckets past PEER_ENTRY_TTL.
    void expire_stale();

   private:
    struct PeerEntry
    {
        std::string compact;  // 6-byte compact ip:port
        std::chrono::steady_clock::time_point last_seen{};
    };

    static constexpr auto PEER_ENTRY_TTL = std::chrono::minutes(30);
    static constexpr size_t MAX_PEERS_PER_HASH = 50;
#ifndef DHT_MAX_INFO_HASHES
    static constexpr size_t MAX_INFO_HASHES = 10000;
#else
    static constexpr size_t MAX_INFO_HASHES = DHT_MAX_INFO_HASHES;
#endif

    struct HashBucket
    {
        std::vector<PeerEntry> peers;
        std::chrono::steady_clock::time_point last_seen{};
    };

    void evict_lru_if_at_capacity();

    std::unordered_map<std::string, HashBucket> buckets_;
    mutable std::mutex mutex_;
};

}  // namespace dht
