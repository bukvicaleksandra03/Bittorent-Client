#include "dht/get_peers_lookup_manager.h"

#include <sstream>

#include "dht/krpc.h"

namespace dht
{

namespace
{

static constexpr auto GET_PEERS_TXN_TTL = std::chrono::seconds(30);
static constexpr auto GET_PEERS_LOOKUP_TTL = std::chrono::minutes(5);

}  // namespace

GetPeersLookupManager::GetPeersLookupManager(RoutingTable& routing_table,
                                             NodeId self_id)
    : routing_table_(routing_table), self_id_(std::move(self_id))
{
}

void GetPeersLookupManager::set_dht_logger(
    std::shared_ptr<logger::Logger> logger)
{
    std::lock_guard<std::mutex> lock(mutex_);
    dht_logger_ = std::move(logger);
}

GetPeersLookupManager::StartResult GetPeersLookupManager::start_or_advance(
    const InfoHash& info_hash)
{
    StartResult result;

    std::vector<RoutingEntry> closest = routing_table_.closest(
        NodeId::from_info_hash(info_hash), RoutingTable::K);

    if (closest.empty())
    {
        result.no_candidates = true;
        log_info("get_peers: no routing-table candidates for " +
                 info_hash.hex());
        return result;
    }

    {
        std::lock_guard<std::mutex> lk(mutex_);
        expire_pending_lookups_locked();
        expire_active_lookups_locked();

        auto it = kademlia_lookups_.find(info_hash);
        if (it != kademlia_lookups_.end())
        {
            if (it->second.has_pending())
            {
                result.skipped_pending = true;
            }
            else if (lookup_should_finish(it->second, info_hash))
            {
                finish_lookup(info_hash);
                result.cleared_exhausted = true;
                result.lookup_completed = true;
            }
            else
            {
                AdvanceResult adv = advance_lookup(info_hash);
                result.outbound = std::move(adv.outbound);
                result.lookup_completed = adv.lookup_completed;
            }
        }

        if (result.outbound.empty() && kademlia_lookups_.count(info_hash) == 0)
        {
            kademlia_lookups_.emplace(
                info_hash, KademliaLookup(info_hash, std::move(closest)));
            result.started_new = true;
            AdvanceResult adv = advance_lookup(info_hash);
            result.outbound = std::move(adv.outbound);
            result.lookup_completed = adv.lookup_completed;
        }
    }

    std::ostringstream oss;
    oss << "get_peers: hash=" << info_hash.hex()
        << " outbound=" << result.outbound.size();
    if (result.started_new)
        oss << " started_new=1";
    if (result.skipped_pending)
        oss << " skipped_pending=1";
    if (result.cleared_exhausted)
        oss << " cleared_exhausted=1";
    log_info(oss.str());

    return result;
}

std::optional<InfoHash> GetPeersLookupManager::info_hash_for_txn(
    const std::string& txn) const
{
    std::lock_guard<std::mutex> lk(mutex_);
    auto pit = pending_lookups_.find(txn);
    if (pit == pending_lookups_.end())
        return std::nullopt;
    return pit->second.first;
}

GetPeersLookupManager::ResponseResult GetPeersLookupManager::on_response(
    const KrpcMessage& msg)
{
    ResponseResult result;
    std::lock_guard<std::mutex> lk(mutex_);

    auto pit = pending_lookups_.find(msg.txn);
    if (pit == pending_lookups_.end())
        return result;

    result.txn_known = true;
    result.info_hash = pit->second.first;

    if (kademlia_lookups_.count(*result.info_hash))
    {
        on_lookup_response(msg, *result.info_hash);
        if (kademlia_lookups_.count(*result.info_hash))
        {
            AdvanceResult adv = advance_lookup(*result.info_hash);
            result.outbound = std::move(adv.outbound);
            result.lookup_completed = adv.lookup_completed;
        }
        else
        {
            result.lookup_completed = true;
        }
    }

    pending_lookups_.erase(pit);
    return result;
}

bool GetPeersLookupManager::has_pending_lookup(const InfoHash& info_hash) const
{
    std::lock_guard<std::mutex> lk(mutex_);
    auto ait = kademlia_lookups_.find(info_hash);
    if (ait != kademlia_lookups_.end() && ait->second.has_pending())
        return true;

    for (const auto& [txn, binding] : pending_lookups_)
    {
        (void)txn;
        if (binding.first == info_hash)
            return true;
    }
    return false;
}

void GetPeersLookupManager::expire_pending_lookups()
{
    std::lock_guard<std::mutex> lk(mutex_);
    expire_pending_lookups_locked();
}

void GetPeersLookupManager::expire_active_lookups()
{
    std::lock_guard<std::mutex> lk(mutex_);
    expire_active_lookups_locked();
}

std::vector<GetPeersLookupManager::OutboundKrpc> GetPeersLookupManager::tick()
{
    std::vector<OutboundKrpc> outbound;
    size_t active_count = 0;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        expire_pending_lookups_locked();

        std::vector<InfoHash> hashes;
        active_count = kademlia_lookups_.size();
        hashes.reserve(active_count);
        for (const auto& [hash, _] : kademlia_lookups_)
            hashes.push_back(hash);

        for (const InfoHash& hash : hashes)
        {
            AdvanceResult adv = advance_lookup(hash);
            outbound.insert(outbound.end(),
                            std::make_move_iterator(adv.outbound.begin()),
                            std::make_move_iterator(adv.outbound.end()));
        }
    }

    log_debug("tick_get_peers_lookups: active=" + std::to_string(active_count) +
              " outbound=" + std::to_string(outbound.size()));

    return outbound;
}

void GetPeersLookupManager::expire_pending_lookups_locked()
{
    const auto now = std::chrono::steady_clock::now();
    size_t expired = 0;
    for (auto it = pending_lookups_.begin(); it != pending_lookups_.end();)
    {
        if (now - it->second.second >= GET_PEERS_TXN_TTL)
        {
            const std::string& txn = it->first;
            const InfoHash& hash = it->second.first;
            auto ait = kademlia_lookups_.find(hash);
            if (ait != kademlia_lookups_.end())
                ait->second.remove_pending_txn(txn);
            it = pending_lookups_.erase(it);
            ++expired;
        }
        else
        {
            ++it;
        }
    }
    log_debug("expire_pending_lookups: expired " + std::to_string(expired) +
              " pending get_peers transaction(s)");
}

void GetPeersLookupManager::expire_active_lookups_locked()
{
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::string> expired_hashes;
    for (auto it = kademlia_lookups_.begin(); it != kademlia_lookups_.end();)
    {
        if (now - it->second.started() >= GET_PEERS_LOOKUP_TTL)
        {
            const InfoHash hash = it->first;
            for (const auto& txn : it->second.pending_txns())
                pending_lookups_.erase(txn);
            expired_hashes.push_back(hash.hex());
            it = kademlia_lookups_.erase(it);
        }
        else
        {
            ++it;
        }
    }

    if (expired_hashes.empty())
    {
        log_info("expire_active_lookups: no kademlia lookups expired");
    }
    else
    {
        std::ostringstream oss;
        oss << "expire_active_lookups: expired " << expired_hashes.size()
            << " kademlia lookup(s):";
        for (const std::string& hash_hex : expired_hashes)
            oss << " " << hash_hex;
        log_info(oss.str());
    }
}

void GetPeersLookupManager::finish_lookup(const InfoHash& info_hash)
{
    auto it = kademlia_lookups_.find(info_hash);
    if (it == kademlia_lookups_.end())
        return;

    for (const auto& txn : it->second.pending_txns())
        pending_lookups_.erase(txn);
    kademlia_lookups_.erase(it);
    log_info("finish_lookup: completed kademlia lookup for " + info_hash.hex());
}

bool GetPeersLookupManager::lookup_should_finish(KademliaLookup& lookup,
                                                 const InfoHash& info_hash)
{
    if (lookup.has_pending() || !lookup.all_candidates_queried())
        return false;

    std::vector<RoutingEntry> closest = routing_table_.closest(
        NodeId::from_info_hash(info_hash), RoutingTable::K);

    if (!closest.empty())
    {
        lookup.merge_nodes(closest, self_id_);
        if (!lookup.all_candidates_queried())
        {
            log_info("lookup replenished from routing table hash=" +
                     info_hash.hex());
            return false;
        }
    }

    return true;
}

GetPeersLookupManager::AdvanceResult GetPeersLookupManager::advance_lookup(
    const InfoHash& info_hash)
{
    log_debug("Advance lookup for " + info_hash.hex());
    AdvanceResult result;

    auto it = kademlia_lookups_.find(info_hash);
    if (it == kademlia_lookups_.end())
    {
        log_info("advance_lookup: no active kademlia lookup for " +
                 info_hash.hex());
        return result;
    }

    KademliaLookup& lookup = it->second;

    if (lookup_should_finish(lookup, info_hash))
    {
        finish_lookup(info_hash);
        result.lookup_completed = true;
        log_info("advance_lookup: exhausted candidates for " + info_hash.hex());
        return result;
    }

    const size_t in_flight = lookup.pending_count();
    if (in_flight >= LOOKUP_ALPHA)
    {
        log_info("advance_lookup: waiting hash=" + info_hash.hex() +
                 " in_flight=" + std::to_string(in_flight) +
                 " alpha=" + std::to_string(LOOKUP_ALPHA));
        return result;
    }

    const size_t can_send = LOOKUP_ALPHA - in_flight;
    std::vector<RoutingEntry> next = lookup.select_next_candidates(can_send);
    for (const auto& node : next)
    {
        const std::string txn = random_txn();
        lookup.add_pending_txn(txn);
        pending_lookups_[txn] = {info_hash, std::chrono::steady_clock::now()};

        result.outbound.push_back(
            {make_get_peers(txn, self_id_, info_hash), node.pa});
    }

    if (result.outbound.empty() && lookup_should_finish(lookup, info_hash))
    {
        finish_lookup(info_hash);
        result.lookup_completed = true;
        log_info("advance_lookup: exhausted candidates for " + info_hash.hex());
    }
    else
    {
        log_info("advance_lookup: hash=" + info_hash.hex() +
                 " sent=" + std::to_string(result.outbound.size()) +
                 " in_flight=" + std::to_string(lookup.pending_count()));
    }

    return result;
}

void GetPeersLookupManager::on_lookup_response(const KrpcMessage& msg,
                                               const InfoHash& info_hash)
{
    auto it = kademlia_lookups_.find(info_hash);
    if (it == kademlia_lookups_.end())
    {
        log_info("on_lookup_response: no active kademlia lookup for " +
                 info_hash.hex() + " txn=" + msg.txn);
        return;
    }

    KademliaLookup& lookup = it->second;
    lookup.remove_pending_txn(msg.txn);
    lookup.merge_nodes(msg.nodes, self_id_);

    if (!msg.values.empty())
    {
        log_info("on_lookup_response: peers found hash=" + info_hash.hex() +
                 " peers=" + std::to_string(msg.values.size()) +
                 " (continuing lookup)");
    }

    if (lookup_should_finish(lookup, info_hash))
    {
        log_info("on_lookup_response: lookup complete hash=" + info_hash.hex());
        finish_lookup(info_hash);
        return;
    }

    log_info("on_lookup_response: continuing hash=" + info_hash.hex() +
             " nodes=" + std::to_string(msg.nodes.size()) +
             " pending=" + std::to_string(lookup.pending_count()));
}

void GetPeersLookupManager::log_info(const std::string& message) const
{
    if (dht_logger_)
        dht_logger_->info("[Get Peers Lookup] " + message);
}

void GetPeersLookupManager::log_debug(const std::string& message) const
{
    if (dht_logger_)
        dht_logger_->debug("[Get Peers Lookup] " + message);
}

}  // namespace dht
