#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "dht/node_id.h"

namespace dht
{

// A single routing-table entry: remote peer ID, address, and last contact time.
struct RoutingEntry
{
    NodeId id;
    std::string ip;  // dotted-decimal IPv4
    uint16_t port{0};

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

// Simplified routing table: a flat list of up to max_size "good" nodes
// sorted by XOR distance to our own ID.
//
// A full Kademlia k-bucket implementation can be added later if needed.
class RoutingTable
{
   public:
    explicit RoutingTable(const NodeId& self_id, size_t max_size = 1000);

    // Add or update a node.  If the table is full, the node is dropped unless
    // it is closer than the furthest existing entry.
    void add(const RoutingEntry& entry);

    // Remove a node by ID (e.g., after it fails to respond).
    void remove(const NodeId& id);

    // Return up to k nodes closest to target (k defaults to 8).
    std::vector<RoutingEntry> closest(const NodeId& target, size_t k = 8) const;

    // Number of nodes currently in the table.
    size_t size() const
    {
        return entries_.size();
    }

    // Return all nodes (useful for persistence / bootstrap refresh).
    const std::vector<RoutingEntry>& all() const
    {
        return entries_;
    }

    const NodeId& self_id() const
    {
        return self_id_;
    }

   private:
    NodeId self_id_;
    size_t max_size_;
    std::vector<RoutingEntry> entries_;
};

}  // namespace dht
