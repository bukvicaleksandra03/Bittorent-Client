#include "dht/kademlia_lookup.h"

#include <algorithm>

namespace dht
{

KademliaLookup::KademliaLookup(std::string info_hash_20,
                               std::vector<RoutingEntry> seed_candidates)
    : info_hash_20_(std::move(info_hash_20)),
      target_(NodeId::from_string(info_hash_20)),
      candidates_(std::move(seed_candidates)),
      started_(std::chrono::steady_clock::now())
{
}

bool KademliaLookup::all_candidates_queried() const
{
    for (const auto& node : candidates_)
    {
        if (queried_.find(node.pa) == queried_.end())
            return false;
    }
    return true;
}

void KademliaLookup::merge_nodes(const std::vector<RoutingEntry>& nodes,
                                 const NodeId& self_id)
{
    for (const auto& n : nodes)
    {
        if (n.id == self_id)
            continue;
        candidates_.push_back(n);
    }

    std::sort(candidates_.begin(),
              candidates_.end(),
              [this](const RoutingEntry& a, const RoutingEntry& b)
              { return (target_ ^ a.id) < (target_ ^ b.id); });

    if (candidates_.size() > RoutingTable::K)
        candidates_.resize(RoutingTable::K);
}

std::vector<RoutingEntry> KademliaLookup::select_next_candidates(size_t count)
{
    std::vector<RoutingEntry> selected;
    for (const auto& node : candidates_)
    {
        if (selected.size() >= count)
            break;

        const PeerAddress& addr = node.pa;
        if (queried_.count(addr))
            continue;

        queried_.insert(addr);
        selected.push_back(node);
    }
    return selected;
}

}  // namespace dht
