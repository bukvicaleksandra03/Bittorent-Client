#include "dht/dht_peer_store.h"

#include "peer_address.h"

namespace dht
{

void DhtPeerStore::expire_stale()
{
    std::lock_guard<std::mutex> lk(mutex_);
    const auto now = std::chrono::steady_clock::now();
    for (auto map_it = buckets_.begin(); map_it != buckets_.end();)
    {
        const auto& peers = map_it->second.peers;

        // The front entry (peers.begin()) is always the most recently
        // seen peer in the bucket. If even that one is past TTL, every other
        // entry (all with an earlier last-seen time) is too, so the whole
        // hash is dead.
        const bool bucket_is_dead =
            peers.empty() || now - peers.begin()->second >= PEER_ENTRY_TTL;

        if (bucket_is_dead)
            map_it = buckets_.erase(map_it);
        else
            ++map_it;
    }
}

void DhtPeerStore::ensure_bucket(const std::string& info_hash)
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (buckets_.get(info_hash) != nullptr)
        return;

    buckets_.put(info_hash, HashBucket{});
}

void DhtPeerStore::upsert(const std::string& info_hash, PeerAddress pa)
{
    std::lock_guard<std::mutex> lk(mutex_);
    const auto now = std::chrono::steady_clock::now();

    HashBucket* bucket = buckets_.get(info_hash);
    if (bucket == nullptr)
        bucket = &buckets_.put(info_hash, HashBucket{});

    // Records a new peer, or refreshes an existing one, promoting it to
    // most-recently-used and evicting the LRU peer once the bucket already
    // holds MAX_PEERS_PER_HASH entries.
    bucket->peers.put(pa, now);
}

std::vector<PeerAddress> DhtPeerStore::live_peers(const std::string& info_hash)
{
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<PeerAddress> result;
    HashBucket* bucket = buckets_.get(info_hash);
    if (bucket == nullptr)
        return result;

    const auto now = std::chrono::steady_clock::now();
    result.reserve(bucket->peers.size());
    for (const auto& [pa, last_seen] : bucket->peers)
    {
        if (now - last_seen < PEER_ENTRY_TTL)
            result.push_back(pa);
    }
    return result;
}

}  // namespace dht
