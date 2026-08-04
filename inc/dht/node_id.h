#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "info_hash.h"

namespace dht
{

// A 160-bit (20-byte) Kademlia node identifier.
// Node IDs are compared using XOR distance per BEP 5.
struct NodeId
{
    std::array<uint8_t, 20> bytes{};

    // Generate a random 20-byte ID at construction time.
    static NodeId random();

    // Construct from a 20-byte raw string (e.g., from a KRPC response).
    // Returns an empty NodeId on failure (wrong length).
    static NodeId from_string(const std::string& s);

    // Construct from a torrent info hash (same 160-bit keyspace as Kademlia).
    static NodeId from_info_hash(const InfoHash& hash);

    // Serialise to a 20-byte raw string for use in KRPC messages.
    std::string to_string() const;

    // XOR distance between two IDs.
    // Smaller result means closer in the Kademlia keyspace.
    NodeId operator^(const NodeId& other) const;

    bool operator<(const NodeId& other) const;
    bool operator==(const NodeId& other) const;
    bool operator!=(const NodeId& other) const;

    // Human-readable hex string (for logging).
    std::string hex() const;

    bool is_zero() const;
};

}  // namespace dht
