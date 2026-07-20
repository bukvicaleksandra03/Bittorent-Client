#include "dht/dht_client.h"

#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>

#include "dht/krpc.h"
#include "net/socket.h"
#include "net/socket_addresses.h"
#include "peer_address.h"

namespace dht
{

// ---------------------------------------------------------------------------
// Bootstrap nodes (well-known public DHT nodes from BEP 5)
// ---------------------------------------------------------------------------
static const std::vector<std::pair<std::string, uint16_t>> BOOTSTRAP_NODES = {
    {"router.bittorrent.com", 6881},
    {"router.utorrent.com", 6881},
    {"dht.transmissionbt.com", 6881},
};

// How often to rotate the token secret (5 minutes).
static constexpr auto TOKEN_ROTATE_INTERVAL = std::chrono::minutes(5);

// How often to run the maintenance loop.
static constexpr auto MAINT_INTERVAL = std::chrono::seconds(60);

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

    recv_thread_ = std::thread([this] { recv_loop(); });
    maint_thread_ = std::thread([this] { maintenance_loop(); });

    bootstrap();
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

    socket_.reset();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

size_t DhtClient::routing_table_size() const
{
    std::lock_guard<std::mutex> lk(table_mutex_);
    return routing_table_.size();
}

void DhtClient::add_krpc_logger(std::shared_ptr<logger::Logger> logger)
{
    if (!logger)
        return;

    std::lock_guard<std::mutex> lk(krpc_loggers_mutex_);
    for (const auto& existing : krpc_loggers_)
    {
        if (existing == logger)
            return;
    }
    krpc_loggers_.push_back(std::move(logger));
}

void DhtClient::log_krpc(const char* direction,
                       const std::string& msg,
                       const std::string& ip,
                       uint16_t port) const
{
    auto parsed = parse_krpc(msg);
    if (parsed)
        log_krpc(direction, *parsed, ip, port);
    else
    {
        std::ostringstream line;
        line << direction << ' ' << ip << ':' << port << " (unparsed, "
             << msg.size() << " bytes)";

        std::lock_guard<std::mutex> lk(krpc_loggers_mutex_);
        for (const auto& logger : krpc_loggers_)
        {
            if (logger)
                logger->info(line.str());
        }
    }
}

void DhtClient::log_krpc(const char* direction,
                       const KrpcMessage& msg,
                       const std::string& ip,
                       uint16_t port) const
{
    std::ostringstream line;
    line << direction << ' ' << ip << ':' << port << ' '
         << format_krpc_summary(msg);

    std::lock_guard<std::mutex> lk(krpc_loggers_mutex_);
    for (const auto& logger : krpc_loggers_)
    {
        if (logger)
            logger->info(line.str());
    }
}

void DhtClient::get_peers(const std::string& info_hash_20)
{
    std::vector<RoutingEntry> closest;
    {
        std::lock_guard<std::mutex> lk(table_mutex_);
        closest = routing_table_.closest(NodeId::from_string(info_hash_20), 8);
    }

    std::string txn = random_txn();
    std::string msg = make_get_peers(txn, self_id_, info_hash_20);

    for (const auto& node : closest)
        send_krpc(msg, node.ip, node.port);
}

void DhtClient::announce(const std::string& info_hash_20, uint16_t port)
{
    std::vector<std::pair<std::string, std::string>> tokens_to_announce;
    {
        std::lock_guard<std::mutex> lk(peers_mutex_);
        for (const auto& [addr_key, token] : received_tokens_)
            tokens_to_announce.push_back({addr_key, token});
    }

    for (const auto& [addr_key, token] : tokens_to_announce)
    {
        // addr_key is "ip:port"
        auto colon = addr_key.rfind(':');
        if (colon == std::string::npos)
            continue;
        std::string ip = addr_key.substr(0, colon);
        uint16_t pt =
            static_cast<uint16_t>(std::stoi(addr_key.substr(colon + 1)));

        std::string txn = random_txn();
        std::string msg =
            make_announce_peer(txn, self_id_, info_hash_20, port, token, true);
        send_krpc(msg, ip, pt);
    }
}

// ---------------------------------------------------------------------------
// Network I/O
// ---------------------------------------------------------------------------

void DhtClient::send_krpc(const std::string& msg,
                        const std::string& ip,
                        uint16_t port)
{
    if (!socket_ || socket_->get_fd() == -1 || ip.empty())
        return;

    log_krpc("SEND ->", msg, ip, port);

    try
    {
        socket_->sendto(msg, IPv4Address(ip, port));
    }
    catch (const std::exception&)
    {
        // Invalid address or send error – silently drop.
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

            std::string src_ip = ipv4->ip();
            uint16_t src_port = ipv4->port();

            auto parsed = parse_krpc(data);
            if (!parsed)
                continue;

            handle_message(*parsed, src_ip, src_port);
        }
        catch (const std::exception&)
        {
            // Timeout or socket error – loop back and check running_.
        }
    }
}

void DhtClient::handle_message(const KrpcMessage& msg,
                             const std::string& src_ip,
                             uint16_t src_port)
{
    log_krpc("RECV <-", msg, src_ip, src_port);

    // Update the routing table with any node we hear from.
    if (!msg.sender_id.is_zero())
    {
        RoutingEntry n;
        n.id = msg.sender_id;
        n.ip = src_ip;
        n.port = src_port;
        n.last_seen = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lk(table_mutex_);
            routing_table_.add(n);
        }
    }

    switch (msg.type)
    {
        case KrpcType::Query:
            if (!msg.query_type)
                break;
            switch (*msg.query_type)
            {
                case KrpcQuery::Ping:
                    on_ping(msg, src_ip, src_port);
                    break;
                case KrpcQuery::FindNode:
                    on_find_node(msg, src_ip, src_port);
                    break;
                case KrpcQuery::GetPeers:
                    on_get_peers(msg, src_ip, src_port);
                    break;
                case KrpcQuery::AnnouncePeer:
                    on_announce_peer(msg, src_ip, src_port);
                    break;
            }
            break;

        case KrpcType::Response:
            on_response(msg, src_ip, src_port);
            break;

        case KrpcType::Error:
            // Nothing to do for now.
            break;
    }
}

// ---------------------------------------------------------------------------
// Query handlers
// ---------------------------------------------------------------------------

void DhtClient::on_ping(const KrpcMessage& msg,
                      const std::string& src_ip,
                      uint16_t src_port)
{
    send_krpc(make_response(msg.txn, self_id_), src_ip, src_port);
}

void DhtClient::on_find_node(const KrpcMessage& msg,
                           const std::string& src_ip,
                           uint16_t src_port)
{
    std::vector<RoutingEntry> closest;
    {
        std::lock_guard<std::mutex> lk(table_mutex_);
        closest = routing_table_.closest(msg.target, 8);
    }
    send_krpc(
        make_nodes_response(msg.txn, self_id_, closest), src_ip, src_port);
}

void DhtClient::on_get_peers(const KrpcMessage& msg,
                           const std::string& src_ip,
                           uint16_t src_port)
{
    rotate_token_if_needed();
    std::string token = current_token();

    // Check if we know any peers for this info_hash.
    std::vector<std::string> known_peers;
    {
        std::lock_guard<std::mutex> lk(peers_mutex_);
        auto it = peer_store_.find(msg.info_hash);
        if (it != peer_store_.end())
            known_peers = it->second;
    }

    if (!known_peers.empty())
    {
        send_krpc(make_peers_response(msg.txn, self_id_, token, known_peers),
                  src_ip,
                  src_port);
    }
    else
    {
        std::vector<RoutingEntry> closest;
        {
            std::lock_guard<std::mutex> lk(table_mutex_);
            closest =
                routing_table_.closest(NodeId::from_string(msg.info_hash), 8);
        }
        send_krpc(make_nodes_response_gp(msg.txn, self_id_, token, closest),
                  src_ip,
                  src_port);
    }
}

void DhtClient::on_announce_peer(const KrpcMessage& msg,
                               const std::string& src_ip,
                               uint16_t src_port)
{
    // Verify the token.
    if (!verify_token(msg.token))
    {
        send_krpc(make_error(msg.txn, 203, "Bad token"), src_ip, src_port);
        return;
    }

    // Store the peer.
    uint16_t peer_port = msg.implied_port ? src_port : msg.peer_port;
    std::string compact = peer_to_compact(src_ip, peer_port);
    {
        std::lock_guard<std::mutex> lk(peers_mutex_);
        peer_store_[msg.info_hash].push_back(compact);
    }

    // Fire the callback so the torrent layer learns about this peer.
    if (peer_cb_)
    {
        // Convert info_hash to hex for a human-readable key.
        std::ostringstream oss;
        oss << std::hex << std::setfill('0');
        for (unsigned char c : msg.info_hash)
            oss << std::setw(2) << static_cast<unsigned>(c);
        peer_cb_(oss.str(), src_ip, peer_port);
    }

    send_krpc(make_response(msg.txn, self_id_), src_ip, src_port);
}

// ---------------------------------------------------------------------------
// Response handler
// ---------------------------------------------------------------------------

void DhtClient::on_response(const KrpcMessage& msg,
                          const std::string& src_ip,
                          uint16_t src_port)
{
    // Add any nodes we received to the routing table.
    {
        std::lock_guard<std::mutex> lk(table_mutex_);
        for (const auto& n : msg.nodes)
            routing_table_.add(n);
    }

    // Store any token for future announce_peer.
    if (!msg.token.empty())
    {
        std::string key = src_ip + ":" + std::to_string(src_port);
        std::lock_guard<std::mutex> lk(peers_mutex_);
        received_tokens_[key] = msg.token;
    }

    // Deliver any peers to the callback.
    if (peer_cb_ && !msg.peers.empty())
    {
        // Reconstruct the info_hash from whatever we last asked about.
        // For now we fire the callback with the raw hex of the sender ID
        // (sufficient for the torrent layer to match by info_hash it queried).
        // A production implementation would correlate by transaction_id.
        for (const auto& compact : msg.peers)
        {
            std::string peer_ip;
            uint16_t peer_port = 0;
            if (compact_to_peer(compact, 0, peer_ip, peer_port))
            {
                // Store the peer in the peer_store_.
                // We don't know which info_hash this came from here without
                // a txn->info_hash map, so we store in all active info_hashes.
                std::lock_guard<std::mutex> lk(peers_mutex_);
                for (auto& [ih, peers] : peer_store_)
                    peers.push_back(compact);

                if (peer_cb_)
                    peer_cb_("", peer_ip, peer_port);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Bootstrap / maintenance
// ---------------------------------------------------------------------------

void DhtClient::bootstrap()
{
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
    catch (const std::exception&)
    {
        return;
    }

    if (ip.empty())
        return;

    std::string txn = random_txn();
    send_krpc(make_ping(txn, self_id_), ip, port);

    // Also send a find_node for our own ID to populate the routing table.
    txn = random_txn();
    send_krpc(make_find_node(txn, self_id_, self_id_), ip, port);
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
        refresh_buckets();
        lk.lock();
    }
}

void DhtClient::refresh_buckets()
{
    // Re-ping each node in the routing table to verify it's still alive.
    std::vector<RoutingEntry> nodes;
    {
        std::lock_guard<std::mutex> lk(table_mutex_);
        nodes = routing_table_.all();
    }

    for (const auto& node : nodes)
    {
        // Only re-ping nodes that haven't been heard from recently.
        auto age = std::chrono::steady_clock::now() - node.last_seen;
        if (age > std::chrono::minutes(10))
        {
            std::string txn = random_txn();
            send_krpc(make_ping(txn, self_id_), node.ip, node.port);
        }
    }
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
