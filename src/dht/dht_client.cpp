#include "dht/dht_client.h"

#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>
#include <string>

#include "dht/krpc.h"
#include "net/socket.h"
#include "net/socket_addresses.h"
#include "peer_address.h"

namespace dht
{

// ---------------------------------------------------------------------------
// Bootstrap nodes (well-known public DHT nodes from BEP 5)
// ---------------------------------------------------------------------------
// libtorrent's public router is the most reliable bootstrap in 2024+.
// Legacy BitTorrent/uTorrent routers often stop responding on UDP/6881.
static const std::vector<std::pair<std::string, uint16_t>> BOOTSTRAP_NODES = {
    {"dht.libtorrent.org", 25401},
    {"dht.libtorrent.org", 6881},
    {"dht.transmissionbt.com", 6881},
    {"router.bittorrent.com", 6881},
    {"router.utorrent.com", 6881},
};

// How often to rotate the token secret (5 minutes).
static constexpr auto TOKEN_ROTATE_INTERVAL = std::chrono::minutes(5);

// How often to run the maintenance loop.
static constexpr auto MAINT_INTERVAL = std::chrono::seconds(60);

// Max K-bucket refreshes per maintenance tick (avoids flooding bootstrap nodes
// when many buckets are empty or stale at once).
static constexpr size_t MAX_BUCKETS_REFRESH_PER_TICK = 3;

// How long an unanswered get_peers query blocks further parallel queries.
static constexpr auto GET_PEERS_TXN_TTL = std::chrono::seconds(30);

// Max wall-clock time for one iterative get_peers lookup before giving up.
static constexpr auto GET_PEERS_LOOKUP_TTL = std::chrono::minutes(5);

// How often recv_loop expires get_peers txns and refills alpha parallelism.
static constexpr auto LOOKUP_TICK_INTERVAL = std::chrono::seconds(5);

// Re-run get_peers (and announce on token) for registered torrents.  Must stay
// below PEER_ENTRY_TTL (30 min) in dht_peer_store.h.
static constexpr auto ANNOUNCE_REFRESH_INTERVAL = std::chrono::minutes(12);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Generate a random token secret (8 bytes, hex-encoded).
static std::string make_token_secret()
{
    static std::mt19937 gen(std::random_device{}());
    static std::uniform_int_distribution<unsigned> dist(0, 255);
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (int i = 0; i < 8; ++i)
        oss << std::setw(2) << dist(gen);
    return oss.str();
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

DhtClient::DhtClient(uint16_t port)
    : self_id_(NodeId::random()),
      requested_port_(port),
      port_(port),
      routing_table_(self_id_),
      current_token_(make_token_secret()),
      prev_token_(make_token_secret()),
      last_token_rotation_(std::chrono::steady_clock::now())
{
}

DhtClient::~DhtClient()
{
    stop();
}

// ---------------------------------------------------------------------------
// start / stop
// ---------------------------------------------------------------------------

void DhtClient::start()
{
    if (running_.load())
        return;

    // Create and bind UDP socket using the abstraction layer.
    socket_ = std::make_unique<UDPSocket>(AF_INET);
    socket_->bind(IPv4Address("0.0.0.0", requested_port_));

    // Discover the actual port (important when requested_port_ == 0).
    auto local = socket_->local_address();
    if (auto* ipv4 = dynamic_cast<IPv4Address*>(local.get()))
        port_ = ipv4->port();

    running_.store(true);
    last_lookup_tick_ = std::chrono::steady_clock::now();

    recv_thread_ = std::thread([this] { recv_loop(); });
    maint_thread_ = std::thread([this] { maintenance_loop(); });

    log_dht_info("listening on 0.0.0.0:" + std::to_string(port_) +
                 " node_id=" + self_id_.hex().substr(0, 8) + "...");
    if (bootstrap_on_start_)
        bootstrap();
}

void DhtClient::set_bootstrap_on_start(bool enabled)
{
    bootstrap_on_start_ = enabled;
}

void DhtClient::set_peer_callback(PeerCallback cb)
{
    std::lock_guard<std::mutex> lk(callback_mutex_);
    peer_cb_ = std::move(cb);
}

void DhtClient::invoke_peer_callback(const std::string& info_hash_hex,
                                     PeerAddress pa)
{
    PeerCallback cb;
    {
        std::lock_guard<std::mutex> lk(callback_mutex_);
        cb = peer_cb_;
    }
    if (cb)
        cb(info_hash_hex, pa);
}

void DhtClient::stop()
{
    if (!running_.exchange(false))
        return;

    {
        std::lock_guard<std::mutex> lk(stop_mutex_);
        stop_cv_.notify_all();
    }

    // Close the fd to unblock recv_loop(), but keep socket_ alive until
    // worker threads have exited so they never dereference a null socket_.
    if (socket_)
        socket_->close();

    if (recv_thread_.joinable())
        recv_thread_.join();
    if (maint_thread_.joinable())
        maint_thread_.join();

    {
        std::lock_guard<std::mutex> lk(registered_mutex_);
        registered_torrents_.clear();
    }

    socket_.reset();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

size_t DhtClient::routing_table_size() const
{
    return routing_table_.size();
}

void DhtClient::set_dht_logger(std::shared_ptr<logger::Logger> logger)
{
    std::lock_guard<std::mutex> lk(dht_logger_mutex_);
    dht_logger_ = std::move(logger);
    routing_table_.set_dht_logger(dht_logger_);
}

void DhtClient::log_krpc(const char* direction,
                         const std::string& msg,
                         PeerAddress pa) const
{
    auto parsed = parse_krpc(msg);
    if (parsed)
        log_krpc(direction, *parsed, pa);
    else
    {
        std::ostringstream line;
        line << "[KRPC] " << direction << ' ' << pa.to_string()
             << " (unparsed, " << msg.size() << " bytes)";

        std::lock_guard<std::mutex> lk(dht_logger_mutex_);
        if (dht_logger_)
            dht_logger_->info(line.str());
    }
}

void DhtClient::log_krpc(const char* direction,
                         const KrpcMessage& msg,
                         PeerAddress pa) const
{
    std::ostringstream line;
    line << "[KRPC] " << direction << ' ' << pa.to_string() << ' '
         << format_krpc_summary(msg);

    std::lock_guard<std::mutex> lk(dht_logger_mutex_);
    if (dht_logger_)
        dht_logger_->info(line.str());
}

void DhtClient::log_dht_info(const std::string& message) const
{
    std::lock_guard<std::mutex> lk(dht_logger_mutex_);
    if (dht_logger_)
        dht_logger_->info("[DHT] " + message);
}

void DhtClient::log_dht_debug(const std::string& message) const
{
    std::lock_guard<std::mutex> lk(dht_logger_mutex_);
    if (dht_logger_)
        dht_logger_->debug("[DHT] " + message);
}

void DhtClient::register_torrent(const std::string& info_hash_20, uint16_t port)
{
    if (info_hash_20.size() != 20)
        return;

    {
        std::lock_guard<std::mutex> lk(registered_mutex_);
        registered_torrents_[info_hash_20] =
            RegisteredTorrent{port, std::chrono::steady_clock::now()};
    }

    if (running_.load())
        get_peers(info_hash_20);
}

void DhtClient::unregister_torrent(const std::string& info_hash_20)
{
    if (info_hash_20.size() != 20)
        return;

    std::lock_guard<std::mutex> lk(registered_mutex_);
    registered_torrents_.erase(info_hash_20);
}

// Starts (or advances) an iterative DHT lookup for peers of the given
// 20-byte info_hash. This is called both for a brand-new torrent and
// repeatedly (e.g. on a timer) to drive an in-progress lookup forward,
// since get_peers is non-blocking: it fires off KRPC queries and returns
// immediately, with results/continuations handled later in
// on_lookup_response.
void DhtClient::get_peers(const std::string& info_hash_20)
{
    if (info_hash_20.size() != 20)
        return;

    // Seed candidates from our own routing table: the K nodes we know of
    // that are closest (by XOR distance) to the info_hash. These are the
    // starting point for the iterative lookup.
    std::vector<RoutingEntry> closest = routing_table_.closest(
        NodeId::from_string(info_hash_20), RoutingTable::K);

    if (closest.empty())
    {
        log_dht_info("get_peers: no routing-table candidates for " +
                     bytes_to_hex(info_hash_20));
        return;
    }

    // Make sure a peer bucket exists for this torrent so peers discovered
    // later have somewhere to be stored.
    peer_store_.ensure_bucket(info_hash_20);

    std::vector<OutboundKrpc> outbound;
    bool started_new = false;
    bool skipped_pending = false;
    bool cleared_exhausted = false;
    {
        std::lock_guard<std::mutex> lk(peers_mutex_);
        // Drop any lookups/transactions that have been outstanding too long
        // before deciding what to do next.
        expire_pending_lookups();
        expire_active_lookups();

        auto it = kademlia_lookups_.find(info_hash_20);
        if (it != kademlia_lookups_.end())
        {
            // A lookup for this info_hash is already running. If we're
            // still waiting on responses from previously sent queries,
            // do nothing this round -- avoid re-querying the same nodes.
            if (it->second.has_pending())
            {
                skipped_pending = true;
            }
            else if (lookup_should_finish(it->second, info_hash_20))
            {
                kademlia_lookups_.erase(it);
                cleared_exhausted = true;
            }
            else if (it->second.all_candidates_queried())
            {
                // Replenish added unqueried candidates; send the next batch.
                outbound = advance_lookup(info_hash_20);
            }
            else
            {
                // There are still unqueried candidates left; send the next
                // batch of queries (up to LOOKUP_ALPHA at a time).
                outbound = advance_lookup(info_hash_20);
            }
        }

        // No lookup is currently active for this info_hash (either this is
        // the first call, or the stale one above was just erased) -- start
        // a fresh iterative lookup seeded with the closest nodes we found.
        if (outbound.empty() && kademlia_lookups_.count(info_hash_20) == 0)
        {
            kademlia_lookups_.emplace(
                info_hash_20, KademliaLookup(info_hash_20, std::move(closest)));
            started_new = true;
            outbound = advance_lookup(info_hash_20);
        }
    }

    // Send the queries outside the lock to avoid holding peers_mutex_
    // while doing network I/O.
    for (const auto& o : outbound)
        send_krpc(o.msg, o.pa);

    std::ostringstream oss;
    oss << "get_peers: hash=" << bytes_to_hex(info_hash_20)
        << " outbound=" << outbound.size();
    if (started_new)
        oss << " started_new=1";
    if (skipped_pending)
        oss << " skipped_pending=1";
    if (cleared_exhausted)
        oss << " cleared_exhausted=1";
    log_dht_info(oss.str());
}

void DhtClient::ping(PeerAddress pa)
{
    send_krpc(make_ping(random_txn(), self_id_), pa);
}

void DhtClient::announce(const std::string& info_hash_20, uint16_t port)
{
    std::vector<std::pair<PeerAddress, std::string>> tokens_to_announce;
    {
        std::lock_guard<std::mutex> lk(peers_mutex_);
        for (const auto& [peer_addr, token] : received_tokens_)
            tokens_to_announce.push_back({peer_addr, token});
    }

    for (const auto& [peer_addr, token] : tokens_to_announce)
    {
        std::string txn = random_txn();
        std::string msg =
            make_announce_peer(txn, self_id_, info_hash_20, port, token, true);
        send_krpc(msg, peer_addr);
    }
}

// ---------------------------------------------------------------------------
// Network I/O
// ---------------------------------------------------------------------------

void DhtClient::send_krpc(const std::string& msg, PeerAddress pa)
{
    if (!socket_ || socket_->get_fd() == -1)
        return;

    log_krpc("SEND ->", msg, pa);

    try
    {
        socket_->sendto(msg, IPv4Address(pa.ip, pa.port));
    }
    catch (const std::exception& e)
    {
        log_dht_info("send failed to " + pa.to_string() + ": " + e.what());
    }
}

void DhtClient::recv_loop()
{
    while (running_.load())
    {
        if (!socket_ || socket_->get_fd() == -1)
            break;

        try
        {
            // recvfrom_with_timeout throws on timeout or error, allowing us
            // to re-check running_ once per second.
            auto [data, addr] = socket_->recvfrom_with_timeout(1000);

            auto* ipv4 = dynamic_cast<IPv4Address*>(addr.get());
            if (!ipv4)
                continue;

            PeerAddress pa(ipv4->ip(), ipv4->port());

            auto parsed = parse_krpc(data);
            if (!parsed)
            {
                if (routing_table_size() == 0)
                {
                    log_dht_info("recv unparsed UDP " +
                                 std::to_string(data.size()) + " bytes from " +
                                 pa.to_string());
                }
                continue;
            }

            handle_message(*parsed, pa);
        }
        catch (const std::exception&)
        {
            // Timeout or socket error – loop back and check running_.
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - last_lookup_tick_ >= LOOKUP_TICK_INTERVAL)
        {
            last_lookup_tick_ = now;
            tick_get_peers_lookups();
        }
    }
}

void DhtClient::handle_message(const KrpcMessage& msg, PeerAddress pa)
{
    log_krpc("RECV <-", msg, pa);

    // Update the routing table with any node we hear from.
    if (!msg.sender_id.is_zero())
    {
        RoutingEntry n;
        n.id = msg.sender_id;
        n.pa = pa;
        n.last_seen = std::chrono::steady_clock::now();
        routing_table_.add(n);
    }

    switch (msg.type)
    {
        case KrpcType::Query:
            if (!msg.query_type)
                break;
            switch (*msg.query_type)
            {
                case KrpcQuery::Ping:
                    on_ping(msg, pa);
                    break;
                case KrpcQuery::FindNode:
                    on_find_node(msg, pa);
                    break;
                case KrpcQuery::GetPeers:
                    on_get_peers(msg, pa);
                    break;
                case KrpcQuery::AnnouncePeer:
                    on_announce_peer(msg, pa);
                    break;
            }
            break;

        case KrpcType::Response:
            on_response(msg, pa);
            break;

        case KrpcType::Error:
            // Nothing to do for now.
            break;
    }
}

// ---------------------------------------------------------------------------
// Query handlers
// ---------------------------------------------------------------------------

void DhtClient::on_ping(const KrpcMessage& msg, PeerAddress pa)
{
    send_krpc(make_response(msg.txn, self_id_), pa);
}

void DhtClient::on_find_node(const KrpcMessage& msg, PeerAddress pa)
{
    std::vector<RoutingEntry> closest = routing_table_.closest(msg.target, 8);
    send_krpc(make_nodes_response(msg.txn, self_id_, closest), pa);
}

void DhtClient::on_get_peers(const KrpcMessage& msg, PeerAddress pa)
{
    rotate_token_if_needed();
    std::string token = current_token();

    // Check if we know any live peers for this info_hash.
    peer_store_.expire_stale();
    const std::vector<PeerAddress> known_peers =
        peer_store_.live_peers(msg.info_hash);

    if (!known_peers.empty())
    {
        send_krpc(make_peers_response(msg.txn, self_id_, token, known_peers),
                  pa);
    }
    else
    {
        std::vector<RoutingEntry> closest =
            routing_table_.closest(NodeId::from_string(msg.info_hash), 8);
        send_krpc(make_nodes_response_gp(msg.txn, self_id_, token, closest),
                  pa);
    }
}

void DhtClient::on_announce_peer(const KrpcMessage& msg, PeerAddress pa)
{
    // Verify the token.
    if (!verify_token(msg.token))
    {
        send_krpc(make_error(msg.txn, 203, "Bad token"), pa);
        return;
    }

    // BEP 5: implied_port=1 -> use UDP source port; else use "port" from
    // message.
    if (!msg.implied_port)
        pa.port = msg.peer_port;

    peer_store_.upsert(msg.info_hash, pa);

    // Fire the callback so the torrent layer learns about this peer.
    invoke_peer_callback(bytes_to_hex(msg.info_hash), pa);

    send_krpc(make_response(msg.txn, self_id_), pa);
}

// ---------------------------------------------------------------------------
// Response handler
// ---------------------------------------------------------------------------

void DhtClient::on_response(const KrpcMessage& msg, PeerAddress pa)
{
    // Add any nodes we received to the routing table.
    for (const auto& n : msg.nodes)
        routing_table_.add(n);

    // Store any token for future announce_peer.
    if (!msg.token.empty())
    {
        std::lock_guard<std::mutex> lk(peers_mutex_);
        received_tokens_[pa] = msg.token;
    }

    maybe_announce_registered(msg, pa);

    // Resolve this response to the torrent lookup that sent it.
    // pending_lookups_ maps txn -> (info_hash, sent_at); advance_lookup
    // registers each outbound get_peers query there so any inbound reply can be
    // routed back.
    std::string info_hash_20;
    // Queued follow-up queries; built under the lock, sent after it is released
    // so we never hold peers_mutex_ during network I/O.
    std::vector<OutboundKrpc> outbound;
    {
        std::lock_guard<std::mutex> lk(peers_mutex_);
        auto pit = pending_lookups_.find(msg.txn);
        if (pit == pending_lookups_.end())
        {
            return;
        }

        info_hash_20 = pit->second.first;

        // If an iterative get_peers lookup is still active for this torrent,
        // fold the reply into KademliaLookup state and maybe send the next
        // batch of queries (up to LOOKUP_ALPHA in flight).
        if (kademlia_lookups_.count(info_hash_20))
        {
            on_lookup_response(msg, info_hash_20);
            // on_lookup_response may finish the lookup (peers found or
            // exhausted); only advance if it is still running.
            if (kademlia_lookups_.count(info_hash_20))
                outbound = advance_lookup(info_hash_20);
        }

        // This transaction id is fully handled; drop the routing entry.
        pending_lookups_.erase(pit);
    }

    // get_peers replies may carry compact peer lists even when the lookup
    // continues (nodes-only responses). Persist peers and notify the torrent
    // layer regardless of whether kademlia_lookups_ still has an entry.
    if (!info_hash_20.empty() && !msg.values.empty())
    {
        const std::string info_hash_hex = bytes_to_hex(info_hash_20);

        for (const auto& compact : msg.values)
        {
            std::string peer_ip;
            uint16_t peer_port = 0;
            if (!compact_to_peer(compact, 0, peer_ip, peer_port))
                continue;

            peer_store_.upsert(info_hash_20, PeerAddress(peer_ip, peer_port));
            invoke_peer_callback(info_hash_hex,
                                 PeerAddress(peer_ip, peer_port));
        }
    }

    for (const auto& o : outbound)
        send_krpc(o.msg, o.pa);
}

// ---------------------------------------------------------------------------
// Bootstrap / maintenance
// ---------------------------------------------------------------------------

void DhtClient::bootstrap()
{
    last_bootstrap_attempt_ = std::chrono::steady_clock::now();
    log_dht_info("bootstrapping (" + std::to_string(BOOTSTRAP_NODES.size()) +
                 " nodes)");
    for (const auto& [host, port] : BOOTSTRAP_NODES)
        ping_bootstrap(host, port);
}

void DhtClient::ping_bootstrap(const std::string& host, uint16_t port)
{
    std::string ip;
    try
    {
        auto addresses = dns_lookup(host, std::to_string(port), SOCK_DGRAM);
        for (const auto& addr : addresses)
        {
            if (addr->domain() == AF_INET)
            {
                ip = static_cast<const IPv4Address&>(*addr).ip();
                break;
            }
        }
    }
    catch (const std::exception& e)
    {
        log_dht_info("bootstrap DNS failed for " + host + ": " + e.what());
        return;
    }

    if (ip.empty())
    {
        log_dht_info("bootstrap DNS returned no IPv4 for " + host);
        return;
    }

    log_dht_info("bootstrap " + host + " -> " + ip + ":" +
                 std::to_string(port));
    std::string txn = random_txn();
    send_krpc(make_ping(txn, self_id_), PeerAddress(ip, port));

    // Also send a find_node for our own ID to populate the routing table.
    txn = random_txn();
    send_krpc(make_find_node(txn, self_id_, self_id_), PeerAddress(ip, port));
}

void DhtClient::maintenance_loop()
{
    std::unique_lock<std::mutex> lk(stop_mutex_);
    while (running_.load())
    {
        if (stop_cv_.wait_for(
                lk, MAINT_INTERVAL, [this] { return !running_.load(); }))
        {
            // Stop requested
            break;
        }

        // Timed out
        if (!running_.load())
            break;

        lk.unlock();
        rotate_token_if_needed();
        {
            std::lock_guard<std::mutex> peers_lk(peers_mutex_);
            expire_active_lookups();
        }
        tick_get_peers_lookups();
        peer_store_.expire_stale();
        refresh_buckets();
        maintain_registered_torrents();

        if (bootstrap_on_start_ && routing_table_size() == 0)
        {
            const auto now = std::chrono::steady_clock::now();
            if (now - last_bootstrap_attempt_ >= BOOTSTRAP_RETRY_INTERVAL)
            {
                log_dht_info("routing table empty; retrying bootstrap");
                bootstrap();
                if (!bootstrap_connectivity_warned_)
                {
                    bootstrap_connectivity_warned_ = true;
                    log_dht_info(
                        "no bootstrap replies received; outbound KRPC works "
                        "but "
                        "inbound UDP may be blocked (VPN/firewall/NAT). Try "
                        "without corporate VPN or allow inbound UDP on port " +
                        std::to_string(port_));
                }
            }
        }

        lk.lock();
    }
}

void DhtClient::expire_pending_lookups()
{
    const auto now = std::chrono::steady_clock::now();
    size_t expired = 0;
    for (auto it = pending_lookups_.begin(); it != pending_lookups_.end();)
    {
        if (now - it->second.second >= GET_PEERS_TXN_TTL)
        {
            const std::string& txn = it->first;
            const std::string& hash = it->second.first;
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
    log_dht_info("expire_pending_lookups: expired " + std::to_string(expired) +
                 " pending get_peers transaction(s)");
}

void DhtClient::expire_active_lookups()
{
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::string> expired_hashes;
    for (auto it = kademlia_lookups_.begin(); it != kademlia_lookups_.end();)
    {
        if (now - it->second.started() >= GET_PEERS_LOOKUP_TTL)
        {
            for (const auto& txn : it->second.pending_txns())
                pending_lookups_.erase(txn);
            expired_hashes.push_back(bytes_to_hex(it->first));
            it = kademlia_lookups_.erase(it);
        }
        else
        {
            ++it;
        }
    }

    if (expired_hashes.empty())
    {
        log_dht_info("expire_active_lookups: no kademlia lookups expired");
    }
    else
    {
        std::ostringstream oss;
        oss << "expire_active_lookups: expired " << expired_hashes.size()
            << " kademlia lookup(s):";
        for (const std::string& hash_hex : expired_hashes)
            oss << " " << hash_hex;
        log_dht_info(oss.str());
    }
}

void DhtClient::tick_get_peers_lookups()
{
    std::vector<OutboundKrpc> outbound;
    size_t active_count = 0;
    {
        std::lock_guard<std::mutex> lk(peers_mutex_);
        expire_pending_lookups();

        std::vector<std::string> hashes;
        active_count = kademlia_lookups_.size();
        hashes.reserve(active_count);
        for (const auto& [hash, _] : kademlia_lookups_)
            hashes.push_back(hash);

        for (const auto& hash : hashes)
        {
            auto batch = advance_lookup(hash);
            outbound.insert(outbound.end(),
                            std::make_move_iterator(batch.begin()),
                            std::make_move_iterator(batch.end()));
        }
    }

    for (const auto& o : outbound)
        send_krpc(o.msg, o.pa);

    log_dht_info(
        "tick_get_peers_lookups: active=" + std::to_string(active_count) +
        " outbound=" + std::to_string(outbound.size()));
}

void DhtClient::finish_lookup(const std::string& info_hash_20)
{
    auto it = kademlia_lookups_.find(info_hash_20);
    if (it == kademlia_lookups_.end())
        return;

    for (const auto& txn : it->second.pending_txns())
        pending_lookups_.erase(txn);
    kademlia_lookups_.erase(it);
    log_dht_info("finish_lookup: completed kademlia lookup for " +
                 bytes_to_hex(info_hash_20));
}

bool DhtClient::lookup_should_finish(KademliaLookup& lookup,
                                     const std::string& info_hash_20)
{
    if (lookup.has_pending() || !lookup.all_candidates_queried())
        return false;

    std::vector<RoutingEntry> closest = routing_table_.closest(
        NodeId::from_string(info_hash_20), RoutingTable::K);

    if (!closest.empty())
    {
        lookup.merge_nodes(closest, self_id_);
        if (!lookup.all_candidates_queried())
        {
            log_dht_info("lookup replenished from routing table hash=" +
                         bytes_to_hex(info_hash_20));
            return false;
        }
    }

    return true;
}

std::vector<DhtClient::OutboundKrpc> DhtClient::advance_lookup(
    const std::string& info_hash_20)
{
    log_dht_debug("Advance lookup for " + bytes_to_hex(info_hash_20));
    std::vector<OutboundKrpc> outbound;

    auto it = kademlia_lookups_.find(info_hash_20);
    if (it == kademlia_lookups_.end())
    {
        log_dht_info("advance_lookup: no active kademlia lookup for " +
                     bytes_to_hex(info_hash_20));
        return outbound;
    }

    KademliaLookup& lookup = it->second;

    if (lookup_should_finish(lookup, info_hash_20))
    {
        kademlia_lookups_.erase(it);
        log_dht_info("advance_lookup: exhausted candidates for " +
                     bytes_to_hex(info_hash_20));
        return outbound;
    }

    const size_t in_flight = lookup.pending_count();
    if (in_flight >= LOOKUP_ALPHA)
    {
        log_dht_info(
            "advance_lookup: waiting hash=" + bytes_to_hex(info_hash_20) +
            " in_flight=" + std::to_string(in_flight) +
            " alpha=" + std::to_string(LOOKUP_ALPHA));
        return outbound;
    }

    const size_t can_send = LOOKUP_ALPHA - in_flight;
    std::vector<RoutingEntry> next = lookup.select_next_candidates(can_send);
    for (const auto& node : next)
    {
        const std::string txn = random_txn();
        lookup.add_pending_txn(txn);
        pending_lookups_[txn] = {info_hash_20,
                                 std::chrono::steady_clock::now()};

        outbound.push_back(
            {make_get_peers(txn, self_id_, info_hash_20), node.pa});
    }

    if (outbound.empty() && lookup_should_finish(lookup, info_hash_20))
    {
        kademlia_lookups_.erase(info_hash_20);
        log_dht_info("advance_lookup: exhausted candidates for " +
                     bytes_to_hex(info_hash_20));
    }
    else
    {
        log_dht_info("advance_lookup: hash=" + bytes_to_hex(info_hash_20) +
                     " sent=" + std::to_string(outbound.size()) +
                     " in_flight=" + std::to_string(lookup.pending_count()));
    }

    return outbound;
}

void DhtClient::on_lookup_response(const KrpcMessage& msg,
                                   const std::string& info_hash_20)
{
    auto it = kademlia_lookups_.find(info_hash_20);
    if (it == kademlia_lookups_.end())
    {
        log_dht_info("on_lookup_response: no active kademlia lookup for " +
                     bytes_to_hex(info_hash_20) + " txn=" + msg.txn);
        return;
    }

    KademliaLookup& lookup = it->second;
    lookup.remove_pending_txn(msg.txn);
    lookup.merge_nodes(msg.nodes, self_id_);

    if (!msg.values.empty())
    {
        log_dht_info("on_lookup_response: peers found hash=" +
                     bytes_to_hex(info_hash_20) +
                     " peers=" + std::to_string(msg.values.size()));
        finish_lookup(info_hash_20);
        return;
    }

    if (lookup_should_finish(lookup, info_hash_20))
    {
        log_dht_info("on_lookup_response: lookup complete without peers hash=" +
                     bytes_to_hex(info_hash_20));
        finish_lookup(info_hash_20);
        return;
    }

    log_dht_info(
        "on_lookup_response: continuing hash=" + bytes_to_hex(info_hash_20) +
        " nodes=" + std::to_string(msg.nodes.size()) +
        " pending=" + std::to_string(lookup.pending_count()));
}

void DhtClient::maintain_registered_torrents()
{
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::string> to_refresh;

    {
        std::lock_guard<std::mutex> reg_lk(registered_mutex_);
        for (const auto& [hash, reg] : registered_torrents_)
        {
            if (now - reg.last_refresh >= ANNOUNCE_REFRESH_INTERVAL)
                to_refresh.push_back(hash);
        }
    }

    for (const auto& hash : to_refresh)
    {
        bool pending = false;
        {
            std::lock_guard<std::mutex> lk(peers_mutex_);
            auto ait = kademlia_lookups_.find(hash);
            if (ait != kademlia_lookups_.end() && ait->second.has_pending())
                pending = true;

            if (!pending)
            {
                for (const auto& [txn, binding] : pending_lookups_)
                {
                    (void)txn;
                    if (binding.first == hash)
                    {
                        pending = true;
                        break;
                    }
                }
            }
        }
        if (pending)
            continue;

        get_peers(hash);

        std::lock_guard<std::mutex> reg_lk(registered_mutex_);
        auto it = registered_torrents_.find(hash);
        if (it != registered_torrents_.end())
            it->second.last_refresh = now;
    }
}

void DhtClient::maybe_announce_registered(const KrpcMessage& msg,
                                          PeerAddress pa)
{
    if (msg.token.empty())
        return;

    std::string info_hash_20;
    {
        std::lock_guard<std::mutex> lk(peers_mutex_);
        auto it = pending_lookups_.find(msg.txn);
        if (it == pending_lookups_.end())
            return;
        info_hash_20 = it->second.first;
    }

    uint16_t listen_port = 0;
    {
        std::lock_guard<std::mutex> lk(registered_mutex_);
        auto it = registered_torrents_.find(info_hash_20);
        if (it == registered_torrents_.end())
            return;
        listen_port = it->second.listen_port;
    }

    announce_to_node(info_hash_20, listen_port, pa, msg.token);
}

void DhtClient::announce_to_node(const std::string& info_hash_20,
                                 uint16_t listen_port,
                                 PeerAddress pa,
                                 const std::string& token)
{
    if (info_hash_20.size() != 20 || token.empty())
        return;

    std::string txn = random_txn();
    send_krpc(make_announce_peer(
                  txn, self_id_, info_hash_20, listen_port, token, true),
              pa);
}

void DhtClient::refresh_buckets()
{
    const auto now = std::chrono::steady_clock::now();
    std::vector<size_t> buckets =
        routing_table_.buckets_needing_refresh(now, std::chrono::minutes(15));

    if (buckets.empty())
        return;

    const size_t bucket_count = buckets.size();
    const size_t start = refresh_bucket_cursor_ % bucket_count;
    const size_t to_refresh =
        std::min(MAX_BUCKETS_REFRESH_PER_TICK, bucket_count);

    for (size_t i = 0; i < to_refresh; ++i)
    {
        const size_t b = buckets[(start + i) % bucket_count];

        NodeId target = routing_table_.random_id_in_bucket(b);
        std::vector<RoutingEntry> closest =
            routing_table_.closest(target, RoutingTable::K);

        size_t sent = 0;
        for (const auto& node : closest)
        {
            if (sent >= 3)
                break;
            const std::string txn = random_txn();
            send_krpc(make_find_node(txn, self_id_, target), node.pa);
            ++sent;
        }
    }

    refresh_bucket_cursor_ = (start + to_refresh) % bucket_count;
}

// ---------------------------------------------------------------------------
// Token management
// ---------------------------------------------------------------------------

std::string DhtClient::current_token() const
{
    std::lock_guard<std::mutex> lk(token_mutex_);
    return current_token_;
}

bool DhtClient::verify_token(const std::string& token) const
{
    std::lock_guard<std::mutex> lk(token_mutex_);
    return (token == current_token_ || token == prev_token_);
}

void DhtClient::rotate_token_if_needed()
{
    std::lock_guard<std::mutex> lk(token_mutex_);
    auto now = std::chrono::steady_clock::now();
    if (now - last_token_rotation_ >= TOKEN_ROTATE_INTERVAL)
    {
        prev_token_ = current_token_;
        current_token_ = make_token_secret();
        last_token_rotation_ = now;
    }
}

}  // namespace dht
