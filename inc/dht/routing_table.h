#pragma once

#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "dht/node_id.h"
#include "logger.h"
#include "peer_address.h"

namespace dht
{

// A single routing-table entry: remote peer ID, address, and last contact time.
struct RoutingEntry
{
    NodeId id;
    PeerAddress pa;
    std::chrono::steady_clock::time_point last_seen{};

    // Is this entry still "good" (heard from within the last 15 minutes)?
    bool is_good() const;

    // Serialise to the 26-byte compact node info used by KRPC.
    // Format: 20-byte node ID + 4-byte IP (big-endian) + 2-byte port
    // (big-endian).
    std::string compact() const;

    // Construct a RoutingEntry from 26-byte compact node info.
    // Returns an empty entry (zero ID) on failure.
    static RoutingEntry from_compact(const std::string& data,
                                     size_t offset = 0);
};

// Kademlia k-bucket routing table (BEP 5).
// 160 buckets, k=8 nodes per bucket, indexed by XOR distance to self_id.
class RoutingTable
{
   public:
    static constexpr size_t NUM_BUCKETS = 160;
    static constexpr size_t K = 8;

    explicit RoutingTable(const NodeId& self_id);

    // Same DHT logger as DhtClient (set via set_dht_logger before start()).
    void set_dht_logger(std::shared_ptr<logger::Logger> logger);

    // Add or update a node in the appropriate k-bucket.
    void add(const RoutingEntry& entry);

    // Remove a node by ID (e.g., after it fails to respond).
    void remove(const NodeId& id);

    // Return up to k nodes closest to target (k defaults to K).
    std::vector<RoutingEntry> closest(const NodeId& target, size_t k = K) const;

    // Number of nodes currently in the table.
    size_t size() const;

    // Return all nodes (useful for persistence / bootstrap refresh).
    std::vector<RoutingEntry> all() const;

    // Bucket index for a node ID relative to self (0..159).
    // Returns NUM_BUCKETS (160) if id == self (no bucket).
    size_t bucket_index(const NodeId& id) const;

    // Generate a random node ID whose XOR distance to self falls in bucket.
    NodeId random_id_in_bucket(size_t bucket) const;

    // Buckets that have not been refreshed recently (empty or stale entries).
    std::vector<size_t> buckets_needing_refresh(
        std::chrono::steady_clock::time_point now,
        std::chrono::minutes max_age = std::chrono::minutes(15)) const;

    const NodeId& self_id() const
    {
        return self_id_;
    }

   private:
    using Bucket = std::array<RoutingEntry, K>;

    NodeId self_id_;
    std::shared_ptr<logger::Logger> dht_logger_;
    std::array<Bucket, NUM_BUCKETS> buckets_{};
    std::array<size_t, NUM_BUCKETS> bucket_sizes_{};

    RoutingEntry* find_in_bucket(size_t bucket, const NodeId& id);
    void touch_bucket_entry(size_t bucket,
                            size_t idx,
                            const RoutingEntry& entry);

    void log_error(const std::string& message) const;
};

}  // namespace dht
