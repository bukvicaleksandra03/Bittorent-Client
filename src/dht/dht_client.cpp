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

// How often to run the maintenance loop.
static constexpr auto MAINT_INTERVAL = std::chrono::seconds(60);

// Max K-bucket refreshes per maintenance tick (avoids flooding bootstrap nodes
// when many buckets are empty or stale at once).
static constexpr size_t MAX_BUCKETS_REFRESH_PER_TICK = 3;

// How often recv_loop expires get_peers txns and refills alpha parallelism.
static constexpr auto LOOKUP_TICK_INTERVAL = std::chrono::seconds(5);

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

DhtClient::DhtClient(uint16_t port)
    : self_id_(NodeId::random()),
      requested_port_(port),
      port_(port),
      routing_table_(self_id_),
      lookup_manager_(routing_table_, self_id_)
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
        announce_coordinator_.clear();
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

size_t DhtClient::live_peer_count(const InfoHash& info_hash) const
{
    return const_cast<DhtPeerStore&>(peer_store_).live_peers(info_hash).size();
}

void DhtClient::log_get_peers_lookup_completed(
    const InfoHash& info_hash) const
{
    log_dht_info("get_peers lookup completed for " + info_hash.hex() + ": " +
                 std::to_string(live_peer_count(info_hash)) + " live peer(s)");
}

void DhtClient::set_dht_logger(std::shared_ptr<logger::Logger> logger)
{
    std::lock_guard<std::mutex> lk(dht_logger_mutex_);
    dht_logger_ = std::move(logger);
    routing_table_.set_dht_logger(dht_logger_);
    lookup_manager_.set_dht_logger(dht_logger_);
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

void DhtClient::register_torrent(const InfoHash& info_hash, uint16_t port)
{
    announce_coordinator_.register_torrent(info_hash, port);

    if (running_.load())
        get_peers(info_hash);
}

void DhtClient::unregister_torrent(const InfoHash& info_hash)
{
    announce_coordinator_.unregister_torrent(info_hash);
}

// Starts (or advances) an iterative DHT lookup for peers of the given
// info_hash. This is called both for a brand-new torrent and
// repeatedly (e.g. on a timer) to drive an in-progress lookup forward,
// since get_peers is non-blocking: it fires off KRPC queries and returns
// immediately, with results/continuations handled later in
// on_lookup_response.
void DhtClient::get_peers(const InfoHash& info_hash)
{
    peer_store_.ensure_bucket(info_hash);

    const auto result = lookup_manager_.start_or_advance(info_hash);
    if (result.no_candidates)
        return;

    announce_coordinator_.mark_initial_lookup_started(info_hash);

    for (const auto& o : result.outbound)
        send_krpc(o.msg, o.pa);

    if (result.lookup_completed)
        log_get_peers_lookup_completed(info_hash);
}

void DhtClient::ping(PeerAddress pa)
{
    send_krpc(make_ping(random_txn(), self_id_), pa);
}

void DhtClient::announce(const InfoHash& info_hash, uint16_t port)
{
    std::vector<AnnounceRequest> requests;
    announce_coordinator_.collect_announce_all(info_hash, port, requests);
    send_announce_requests(requests);
}

void DhtClient::send_announce_requests(
    const std::vector<AnnounceRequest>& requests)
{
    for (const auto& req : requests)
    {
        if (req.token.empty())
            continue;

        const std::string txn = random_txn();
        send_krpc(make_announce_peer(txn,
                                     self_id_,
                                     req.info_hash,
                                     req.listen_port,
                                     req.token,
                                     true),
                  req.pa);
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
            for (const auto& o : lookup_manager_.tick())
                send_krpc(o.msg, o.pa);
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
    if (!msg.info_hash)
        return;

    token_rotator_.rotate_if_needed();
    const std::string token = token_rotator_.current_token();
    const InfoHash& info_hash = *msg.info_hash;

    // Check if we know any live peers for this info_hash.
    peer_store_.expire_stale();
    const std::vector<PeerAddress> known_peers =
        peer_store_.live_peers(info_hash);

    if (!known_peers.empty())
    {
        send_krpc(make_peers_response(msg.txn, self_id_, token, known_peers),
                  pa);
    }
    else
    {
        std::vector<RoutingEntry> closest =
            routing_table_.closest(NodeId::from_info_hash(info_hash), 8);
        send_krpc(make_nodes_response_gp(msg.txn, self_id_, token, closest),
                  pa);
    }
}

void DhtClient::on_announce_peer(const KrpcMessage& msg, PeerAddress pa)
{
    if (!msg.info_hash)
        return;

    // Verify the token.
    if (!token_rotator_.verify_token(msg.token))
    {
        send_krpc(make_error(msg.txn, 203, "Bad token"), pa);
        return;
    }

    const InfoHash& info_hash = *msg.info_hash;

    // BEP 5: implied_port=1 -> use UDP source port; else use "port" from
    // message.
    if (!msg.implied_port)
        pa.port = msg.peer_port;

    peer_store_.upsert(info_hash, pa);

    // Fire the callback so the torrent layer learns about this peer.
    invoke_peer_callback(info_hash.hex(), pa);

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

    std::vector<AnnounceRequest> announce_requests;
    if (!msg.token.empty())
    {
        if (const auto info_hash_for_announce =
                lookup_manager_.info_hash_for_txn(msg.txn))
        {
            announce_coordinator_.on_get_peers_response(
                *info_hash_for_announce, pa, msg.token, announce_requests);
        }
    }

    const auto lookup_result = lookup_manager_.on_response(msg);
    if (!lookup_result.txn_known)
    {
        send_announce_requests(announce_requests);
        return;
    }

    const std::optional<InfoHash>& lookup_hash = lookup_result.info_hash;

    if (lookup_hash && !msg.values.empty())
    {
        const std::string info_hash_hex = lookup_hash->hex();

        for (const auto& compact : msg.values)
        {
            std::string peer_ip;
            uint16_t peer_port = 0;
            if (!compact_to_peer(compact, 0, peer_ip, peer_port))
                continue;

            peer_store_.upsert(*lookup_hash, PeerAddress(peer_ip, peer_port));
            invoke_peer_callback(info_hash_hex,
                                 PeerAddress(peer_ip, peer_port));
        }
    }

    for (const auto& o : lookup_result.outbound)
        send_krpc(o.msg, o.pa);

    if (lookup_result.lookup_completed && lookup_hash)
        log_get_peers_lookup_completed(*lookup_hash);

    send_announce_requests(announce_requests);

    try_pending_initial_lookups();
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
        token_rotator_.rotate_if_needed();
        lookup_manager_.expire_active_lookups();
        for (const auto& o : lookup_manager_.tick())
            send_krpc(o.msg, o.pa);
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

void DhtClient::try_pending_initial_lookups()
{
    if (routing_table_size() == 0)
        return;

    for (const InfoHash& hash :
         announce_coordinator_.torrents_needing_initial_lookup())
    {
        if (lookup_manager_.has_pending_lookup(hash))
            continue;

        get_peers(hash);
    }
}

void DhtClient::maintain_registered_torrents()
{
    try_pending_initial_lookups();

    const auto now = std::chrono::steady_clock::now();
    const std::vector<InfoHash> to_refresh =
        announce_coordinator_.torrents_needing_refresh(now);

    for (const InfoHash& hash : to_refresh)
    {
        if (lookup_manager_.has_pending_lookup(hash))
            continue;

        get_peers(hash);
        announce_coordinator_.mark_refreshed(hash, now);
    }
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

}  // namespace dht
