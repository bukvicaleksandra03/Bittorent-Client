#pragma once

#include <chrono>
#include <set>
#include <string>
#include <vector>

#include "dht/node_id.h"
#include "dht/routing_table.h"
#include "peer_address.h"

namespace dht
{

// ---------------------------------------------------------------------------
// KademliaLookup – state for a single iterative Kademlia get_peers lookup
// (BEP 5).
//
// An iterative lookup queries the alpha closest known nodes to a target ID
// in parallel, merges any nodes discovered from their responses back into
// the candidate list (kept sorted by XOR distance and capped to K), and
// repeats until every candidate has been queried and no queries remain in
// flight, or peers are found and the caller ends the lookup early.
//
// This class only tracks the *shape* of one lookup (candidates, who's been
// queried, which of its own transaction ids are still outstanding). It does
// not perform network I/O and does not own transaction-id -> info_hash
// routing: DhtClient still owns that (pending_lookups_) since a single flat
// map is needed to route any inbound KRPC response to the right lookup,
// regardless of which lookup it belongs to.
// ---------------------------------------------------------------------------
class KademliaLookup
{
   public:
    KademliaLookup(std::string info_hash_20,
                   std::vector<RoutingEntry> seed_candidates);

    const std::string& info_hash() const
    {
        return info_hash_20_;
    }

    const NodeId& target() const
    {
        return target_;
    }

    // When this lookup was started; used by the caller to expire lookups
    // that have stalled (no response for too long).
    std::chrono::steady_clock::time_point started() const
    {
        return started_;
    }

    // True once every known candidate has already been sent a query.
    bool all_candidates_queried() const;

    // True while one or more of our queries are still awaiting a response.
    bool has_pending() const
    {
        return !pending_txns_.empty();
    }

    size_t pending_count() const
    {
        return pending_txns_.size();
    }

    // Every transaction id sent by this lookup that hasn't been answered (or
    // expired) yet. Used by DhtClient to clean up its own txn -> info_hash
    // map when this lookup is torn down or a txn times out.
    const std::set<std::string>& pending_txns() const
    {
        return pending_txns_;
    }

    void add_pending_txn(const std::string& txn)
    {
        pending_txns_.insert(txn);
    }

    void remove_pending_txn(const std::string& txn)
    {
        pending_txns_.erase(txn);
    }

    // Merge freshly-discovered nodes (e.g. from a get_peers reply) into the
    // candidate list, re-sort by XOR distance to the target, and cap the
    // list to the K closest so it doesn't grow unbounded.
    void merge_nodes(const std::vector<RoutingEntry>& nodes,
                     const NodeId& self_id);

    // Pick up to `count` not-yet-queried candidates (closest first), mark
    // them as queried, and return them so the caller can fire off queries.
    std::vector<RoutingEntry> select_next_candidates(size_t count);

   private:
    std::string info_hash_20_;
    NodeId target_;
    std::vector<RoutingEntry> candidates_;
    std::set<PeerAddress> queried_;
    std::set<std::string> pending_txns_;
    std::chrono::steady_clock::time_point started_;
};

}  // namespace dht
