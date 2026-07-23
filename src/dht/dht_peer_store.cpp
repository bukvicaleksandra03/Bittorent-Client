#include "dht/dht_peer_store.h"

#include <algorithm>

namespace dht
{

void DhtPeerStore::expire_stale()
{
    std::lock_guard<std::mutex> lk(mutex_);
    const auto now = std::chrono::steady_clock::now();
    for (auto map_it = buckets_.begin(); map_it != buckets_.end();)
    {
        auto& peers = map_it->second.peers;
        peers.erase(
            std::remove_if(peers.begin(),
                           peers.end(),
                           [&](const PeerEntry& e)
                           { return now - e.last_seen >= PEER_ENTRY_TTL; }),
            peers.end());

        if (peers.empty())
            map_it = buckets_.erase(map_it);
        else
            ++map_it;
    }
}

void DhtPeerStore::evict_lru_if_at_capacity()
{
    if (buckets_.size() < MAX_INFO_HASHES)
        return;

    auto lru = buckets_.end();
    for (auto it = buckets_.begin(); it != buckets_.end(); ++it)
    {
        if (lru == buckets_.end() ||
            it->second.last_seen < lru->second.last_seen)
            lru = it;
    }

    if (lru != buckets_.end())
        buckets_.erase(lru);
}

void DhtPeerStore::ensure_bucket(const std::string& info_hash)
{
    std::lock_guard<std::mutex> lk(mutex_);
    const auto now = std::chrono::steady_clock::now();
    auto it = buckets_.find(info_hash);
    if (it != buckets_.end())
    {
        it->second.last_seen = now;
        return;
    }

    evict_lru_if_at_capacity();
    buckets_.emplace(info_hash, HashBucket{{}, now});
}

void DhtPeerStore::upsert(const std::string& info_hash,
                          const std::string& compact)
{
    std::lock_guard<std::mutex> lk(mutex_);
    const auto now = std::chrono::steady_clock::now();
    auto it = buckets_.find(info_hash);
    if (it == buckets_.end())
    {
        evict_lru_if_at_capacity();
        it = buckets_.emplace(info_hash, HashBucket{{}, now}).first;
    }
    else
    {
        it->second.last_seen = now;
    }

    auto& peers = it->second.peers;

    for (auto& entry : peers)
    {
        if (entry.compact == compact)
        {
            entry.last_seen = now;
            return;
        }
    }

    if (peers.size() >= MAX_PEERS_PER_HASH)
    {
        auto oldest = peers.begin();
        for (auto peer_it = peers.begin() + 1; peer_it != peers.end();
             ++peer_it)
        {
            if (peer_it->last_seen < oldest->last_seen)
                oldest = peer_it;
        }
        *oldest = PeerEntry{compact, now};
        return;
    }

    peers.push_back(PeerEntry{compact, now});
}

std::vector<std::string> DhtPeerStore::live_peers(const std::string& info_hash)
{
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<std::string> result;
    const auto it = buckets_.find(info_hash);
    if (it == buckets_.end())
        return result;

    const auto now = std::chrono::steady_clock::now();
    it->second.last_seen = now;

    result.reserve(it->second.peers.size());
    for (const auto& entry : it->second.peers)
    {
        if (now - entry.last_seen < PEER_ENTRY_TTL)
            result.push_back(entry.compact);
    }
    return result;
}

}  // namespace dht
