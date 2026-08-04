#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "info_hash.h"
#include "logger.h"
#include "dht/kademlia_lookup.h"
#include "dht/krpc.h"
#include "dht/node_id.h"
#include "dht/routing_table.h"
#include "peer_address.h"

namespace dht
{

// Orchestrates iterative get_peers (Kademlia) lookups: one KademliaLookup per
// info hash, transaction routing, and parallel query batching. Does not perform
// UDP I/O; DhtClient sends outbound KRPC and stores discovered peers.
//
// Stack:
//   DhtClient (UDP I/O, routing table, peer store, callbacks)
//     └── GetPeersLookupManager (multi-lookup orchestration, txn routing)
//           └── KademliaLookup (per-info-hash candidates, queried, pending)
//
// Data:
//   kademlia_lookups_  InfoHash -> KademliaLookup (one active lookup per hash)
//   pending_lookups_   txn -> (InfoHash, timestamp) (route inbound replies)
//
// Lookup flow:
//
//                    start_or_advance(info_hash)
//                              |
//              +---------------+---------------+
//              v               v               v
//        no_candidates   skipped_pending   advance_lookup
//              |               |               |
//              v               v               v
//           [stop]          [wait]      send up to alpha get_peers
//                                              |
//                                              v
//                                    pending_lookups_[txn] = hash
//                                              |
//                         UDP reply -----------+|
//                                              v
//                                       on_response(msg)
//                                              |
//                         +--------------------+--------------------+
//                         v                    v                    v
//                   values found        nodes only           txn expired
//                         |                    |                    |
//                         v                    v                    v
//                  finish_lookup         advance_lookup      expire (tick)
//                  peers -> store         more queries
class GetPeersLookupManager
{
   public:
    struct OutboundKrpc
    {
        std::string msg;
        PeerAddress pa;
    };

    struct StartResult
    {
        std::vector<OutboundKrpc> outbound;
        bool started_new = false;
        bool skipped_pending = false;
        bool cleared_exhausted = false;
        bool no_candidates = false;
    };

    struct ResponseResult
    {
        bool txn_known = false;
        std::optional<InfoHash> info_hash;
        std::vector<OutboundKrpc> outbound;
    };

    GetPeersLookupManager(RoutingTable& routing_table, NodeId self_id);

    // Same DHT logger as DhtClient (set via set_dht_logger before start()).
    void set_dht_logger(std::shared_ptr<logger::Logger> logger);

    // Start a new lookup or advance an in-progress one for info_hash.
    StartResult start_or_advance(const InfoHash& info_hash);

    // Resolve txn -> info_hash without removing the binding (for announce).
    std::optional<InfoHash> info_hash_for_txn(const std::string& txn) const;

    // Handle an inbound get_peers response; removes the txn binding when known.
    ResponseResult on_response(const KrpcMessage& msg);

    // True if this hash has in-flight queries or pending txn bindings.
    bool has_pending_lookup(const InfoHash& info_hash) const;

    void expire_pending_lookups();
    void expire_active_lookups();

    // Expire stale txns and advance all active lookups (recv-loop tick).
    std::vector<OutboundKrpc> tick();

   private:
    void finish_lookup(const InfoHash& info_hash);

    bool lookup_should_finish(KademliaLookup& lookup,
                              const InfoHash& info_hash);

    std::vector<OutboundKrpc> advance_lookup(const InfoHash& info_hash);

    void on_lookup_response(const KrpcMessage& msg, const InfoHash& info_hash);

    void expire_pending_lookups_locked();
    void expire_active_lookups_locked();

    void log_info(const std::string& message) const;
    void log_debug(const std::string& message) const;

    RoutingTable& routing_table_;
    NodeId self_id_;
    std::shared_ptr<logger::Logger> dht_logger_;

    std::unordered_map<
        std::string,
        std::pair<InfoHash, std::chrono::steady_clock::time_point>>
        pending_lookups_;

    std::unordered_map<InfoHash, KademliaLookup> kademlia_lookups_;

    static constexpr size_t LOOKUP_ALPHA = 6;

    mutable std::mutex mutex_;
};

}  // namespace dht
