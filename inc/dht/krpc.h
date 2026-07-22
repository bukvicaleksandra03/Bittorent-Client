#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "dht/node_id.h"
#include "dht/routing_table.h"

namespace dht
{

// ---------------------------------------------------------------------------
// KRPC message builders
//
// All functions return a bencoded string ready to be sent over UDP.
// The transaction_id is typically a 2-byte random string that the caller
// picks and stores so it can match responses to requests.
// ---------------------------------------------------------------------------

// q=ping  ->  d1:ad2:id20:...e1:q4:ping1:t2:..1:y1:qe
std::string make_ping(const std::string& txn, const NodeId& self_id);

// q=find_node
std::string make_find_node(const std::string& txn,
                           const NodeId& self_id,
                           const NodeId& target);

// q=get_peers
std::string make_get_peers(const std::string& txn,
                           const NodeId& self_id,
                           const std::string& info_hash_20);

// q=announce_peer
std::string make_announce_peer(const std::string& txn,
                               const NodeId& self_id,
                               const std::string& info_hash_20,
                               uint16_t port,
                               const std::string& token,
                               bool implied_port = false);

// r=pong (response to ping / find_node / announce_peer)
std::string make_response(const std::string& txn, const NodeId& self_id);

// r=find_node response (includes compact node list)
std::string make_nodes_response(const std::string& txn,
                                const NodeId& self_id,
                                const std::vector<RoutingEntry>& nodes);

// r=get_peers response – either compact peer list or compact node list
std::string make_peers_response(const std::string& txn,
                                const NodeId& self_id,
                                const std::string& token,
                                const std::vector<std::string>& compact_peers);

std::string make_nodes_response_gp(const std::string& txn,
                                   const NodeId& self_id,
                                   const std::string& token,
                                   const std::vector<RoutingEntry>& nodes);

// Generic error response
std::string make_error(const std::string& txn,
                       int code,
                       const std::string& msg);

// ---------------------------------------------------------------------------
// KRPC message parser
// ---------------------------------------------------------------------------

enum class KrpcType
{
    Query,
    Response,
    Error
};

enum class KrpcQuery
{
    Ping,
    FindNode,
    GetPeers,
    AnnouncePeer
};

struct KrpcMessage
{
    KrpcType type{KrpcType::Response};
    std::string txn;  // transaction_id

    // Query fields (type == Query)
    std::optional<KrpcQuery> query_type;
    NodeId sender_id;
    NodeId target;             // find_node
    std::string info_hash;     // get_peers / announce_peer (20 bytes)
    uint16_t peer_port{0};     // announce_peer
    std::string token;         // announce_peer / get_peers response
    bool implied_port{false};  // announce_peer

    // Response fields
    std::vector<RoutingEntry> nodes;       // unique compact nodes (nodes / find_node)
    std::vector<size_t> node_counts;       // wire repeat count per nodes[i]
    std::vector<std::string> peers;        // compact peer list (get_peers)

    // Error fields
    int error_code{0};
    std::string error_msg;
};

// Parse a raw UDP payload into a KrpcMessage.
// Returns std::nullopt if the payload is malformed.
std::optional<KrpcMessage> parse_krpc(const std::string& data);

// Generate a 2-byte random transaction ID.
std::string random_txn();

// One-line summary for DHT/KRPC logging (query type, txn, counts, etc.).
std::string format_krpc_summary(const KrpcMessage& msg);

}  // namespace dht
