#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "dht/dht_peer_store.h"
#include "dht/krpc.h"
#include "dht/node_id.h"
#include "dht/routing_table.h"
#include "logger.h"

class UDPSocket;

namespace dht
{

// Callback fired whenever a get_peers lookup yields live peers.
// Arguments: info_hash (hex string), ip, port.
using PeerCallback = std::function<void(
    const std::string& info_hash_hex, const std::string& ip, uint16_t port)>;

// ---------------------------------------------------------------------------
// DhtClient – the public interface to the DHT subsystem.
//
// Usage:
//   DhtClient node(6881);
//   node.set_peer_callback(cb);
//   node.start();
//   node.register_torrent(info_hash_20, listen_port);
//   // maintenance_loop re-runs get_peers and announce_peer on a schedule
//   node.unregister_torrent(info_hash_20);
//   node.stop();
// ---------------------------------------------------------------------------
class DhtClient
{
   public:
    // port: UDP port to listen on (0 = OS-assigned).
    explicit DhtClient(uint16_t port = 6881);
    ~DhtClient();

    // Non-copyable, non-movable (owns a thread and a socket fd).
    DhtClient(const DhtClient&) = delete;
    DhtClient& operator=(const DhtClient&) = delete;

    // Start the DHT node.  Binds the UDP socket, bootstraps from known nodes,
    // and launches the background I/O and maintenance threads.
    void start();

    // When false, start() and maintenance will not contact public bootstrap
    // nodes (for loopback/unit tests).  Must be set before start().
    void set_bootstrap_on_start(bool enabled);

    // Gracefully stop the background threads and close the socket.
    void stop();

    bool is_running() const
    {
        return running_.load();
    }

    // Register a callback that fires when peers are found for a torrent.
    void set_peer_callback(PeerCallback cb);

    // Start (or refresh) a get_peers lookup for the given 20-byte info hash.
    void get_peers(const std::string& info_hash_20);

    // Register an active torrent for periodic DHT peer discovery and
    // announce_peer refresh (info_hash_20 must be exactly 20 raw bytes).
    void register_torrent(const std::string& info_hash_20, uint16_t port);

    // Stop periodic refresh for this torrent.
    void unregister_torrent(const std::string& info_hash_20);

    // Send a ping to a specific node (useful for loopback bootstrap / tests).
    void ping(const std::string& ip, uint16_t port);

    // Announce to every node that has returned a token.  Prefer
    // register_torrent() for long-lived seeding sessions.
    void announce(const std::string& info_hash_20, uint16_t port);

    // Read-only view of our node ID (useful for logging / integration tests).
    const NodeId& self_id() const
    {
        return self_id_;
    }

    // Number of nodes currently in the routing table.
    size_t routing_table_size() const;

    // Shared DHT file logger (e.g. logs/dht.log from SessionManager).
    // Call before start(); routing table uses the same instance.
    void set_dht_logger(std::shared_ptr<logger::Logger> logger);

   private:
    // ---- Network I/O -------------------------------------------------------

    void send_krpc(const std::string& msg,
                   const std::string& ip,
                   uint16_t port);

    void log_krpc(const char* direction,
                  const std::string& msg,
                  const std::string& ip,
                  uint16_t port) const;

    void log_krpc(const char* direction,
                  const KrpcMessage& msg,
                  const std::string& ip,
                  uint16_t port) const;

    // Background thread: receive datagrams and dispatch them.
    void recv_loop();

    // Dispatch a parsed KRPC message.
    void handle_message(const KrpcMessage& msg,
                        const std::string& src_ip,
                        uint16_t src_port);

    // Handlers for each KRPC type.
    void on_ping(const KrpcMessage& msg,
                 const std::string& src_ip,
                 uint16_t src_port);
    void on_find_node(const KrpcMessage& msg,
                      const std::string& src_ip,
                      uint16_t src_port);
    void on_get_peers(const KrpcMessage& msg,
                      const std::string& src_ip,
                      uint16_t src_port);
    void on_announce_peer(const KrpcMessage& msg,
                          const std::string& src_ip,
                          uint16_t src_port);
    void on_response(const KrpcMessage& msg,
                     const std::string& src_ip,
                     uint16_t src_port);

    // ---- Bootstrap / maintenance ------------------------------------------

    void bootstrap();
    void maintenance_loop();

    // Ping one bootstrap node to seed the routing table.
    void ping_bootstrap(const std::string& host, uint16_t port);

    // Refresh buckets that haven't been touched recently.
    void refresh_buckets();

    // Drop get_peers transaction bindings that have expired.
    void expire_pending_lookups();

    // Drop stalled iterative get_peers lookups.
    void expire_active_lookups();

    // Periodic get_peers + announce_peer for registered torrents.
    void maintain_registered_torrents();

    // ---- Iterative get_peers (Kademlia) ------------------------------------

    struct LookupState
    {
        std::string info_hash_20;
        NodeId target;
        std::vector<RoutingEntry> candidates;
        std::set<std::string> queried;
        std::set<std::string> pending_txns;
        std::chrono::steady_clock::time_point started{};
    };

    struct OutboundKrpc
    {
        std::string msg;
        std::string ip;
        uint16_t port{0};
    };

    static std::string addr_key(const std::string& ip, uint16_t port);

    void merge_nodes_into_lookup(LookupState& state,
                                 const std::vector<RoutingEntry>& nodes);

    bool all_closest_queried(const LookupState& state) const;

    void finish_lookup(const std::string& info_hash_20);

    // Send up to alpha parallel get_peers queries for an active lookup.
    std::vector<OutboundKrpc> advance_lookup(const std::string& info_hash_20);

    void on_lookup_response(const KrpcMessage& msg,
                            const std::string& info_hash_20);

    // If msg.txn belongs to a registered torrent, announce to the responder.
    void maybe_announce_registered(const KrpcMessage& msg,
                                   const std::string& src_ip,
                                   uint16_t src_port);

    void announce_to_node(const std::string& info_hash_20,
                          uint16_t listen_port,
                          const std::string& ip,
                          uint16_t node_port,
                          const std::string& token);

    void log_dht_info(const std::string& message) const;

    // ---- Token management (Step 6) ----------------------------------------
    //
    // Tokens are short secrets returned in get_peers responses.  A remote
    // node must echo back the token in a subsequent announce_peer request to
    // prove it recently contacted us.  We rotate tokens every 5 minutes so
    // old tokens are only valid for ≤10 minutes.

    std::string current_token() const;
    bool verify_token(const std::string& token) const;
    void rotate_token_if_needed();

    // ---- State -------------------------------------------------------------

    NodeId self_id_;           // this node's identity in the Kademlia keyspace
    uint16_t requested_port_;  // port passed to the constructor (0 = ephemeral)
    uint16_t port_;  // actual port currently bound (filled in by start())
    std::unique_ptr<UDPSocket> socket_;

    mutable std::mutex table_mutex_;
    RoutingTable routing_table_;

    DhtPeerStore peer_store_;
    // tokens returned by remote nodes keyed by "ip:port"
    std::unordered_map<std::string, std::string> received_tokens_;
    // KRPC transaction id -> info_hash (20 bytes) for in-flight get_peers
    // lookups.
    std::unordered_map<
        std::string,
        std::pair<std::string, std::chrono::steady_clock::time_point>>
        pending_lookups_;

    // One iterative Kademlia lookup per info hash (keyed by 20-byte hash).
    std::unordered_map<std::string, LookupState> active_lookups_;

    static constexpr size_t LOOKUP_ALPHA = 3;

    // Torrents we are actively sharing (seed/leech) that must stay visible in
    // remote DHT peer stores. BEP 5 peer entries expire after ~30 minutes
    // unless refreshed via announce_peer; ping alone only maintains the routing
    // table. listen_port is sent with each announce; last_refresh throttles
    // periodic get_peers lookups (maintain_registered_torrents) that obtain
    // fresh tokens.
    struct RegisteredTorrent
    {
        uint16_t listen_port;
        std::chrono::steady_clock::time_point last_refresh;
    };

    std::unordered_map<std::string, RegisteredTorrent> registered_torrents_;
    mutable std::mutex registered_mutex_;
    mutable std::mutex peers_mutex_;

    // Token rotation
    mutable std::mutex token_mutex_;
    std::string current_token_;
    std::string prev_token_;
    std::chrono::steady_clock::time_point last_token_rotation_{};

    std::atomic<bool> running_{false};

    // Wakes maintenance_loop() from wait_for() when stop() runs, so join() does
    // not block for the full maintenance interval (60s).
    std::mutex stop_mutex_;
    std::condition_variable stop_cv_;

    std::thread recv_thread_;
    std::thread maint_thread_;

    std::chrono::steady_clock::time_point last_bootstrap_attempt_{};
    bool bootstrap_connectivity_warned_{false};
    bool bootstrap_on_start_{true};
    static constexpr auto BOOTSTRAP_RETRY_INTERVAL = std::chrono::minutes(2);

    PeerCallback peer_cb_;
    mutable std::mutex callback_mutex_;

    void invoke_peer_callback(const std::string& info_hash_hex,
                              const std::string& ip,
                              uint16_t port);

    // Protects dht_logger_ assignment; log calls rely on Logger::m_log_mutex.
    mutable std::mutex dht_logger_mutex_;
    std::shared_ptr<logger::Logger> dht_logger_;
};

}  // namespace dht
