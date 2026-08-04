#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "dht/announce_coordinator.h"
#include "info_hash.h"
#include "dht/dht_peer_store.h"
#include "dht/kademlia_lookup.h"
#include "dht/krpc.h"
#include "dht/token_secret_rotator.h"
#include "dht/node_id.h"
#include "dht/routing_table.h"
#include "logger.h"
#include "peer_address.h"

class UDPSocket;

namespace dht
{

// Callback fired whenever a get_peers lookup yields live peers.
// Arguments: info_hash (hex string), peer address.
using PeerCallback =
    std::function<void(const std::string& info_hash_hex, PeerAddress pa)>;

// ---------------------------------------------------------------------------
// DhtClient – the public interface to the DHT subsystem.
//
// Usage:
//   DhtClient node(6881);
//   node.set_peer_callback(cb);
//   node.start();
//   node.register_torrent(info_hash, listen_port);
//   // maintenance_loop re-runs get_peers and announce_peer on a schedule
//   node.unregister_torrent(info_hash);
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

    // Start (or refresh) a get_peers lookup for the given info hash.
    void get_peers(const InfoHash& info_hash);

    // Register an active torrent for periodic DHT announce_peer refresh.
    void register_torrent(const InfoHash& info_hash, uint16_t port);

    // Stop periodic refresh for this torrent.
    void unregister_torrent(const InfoHash& info_hash);

    // Send a ping to a specific node (useful for loopback bootstrap / tests).
    void ping(PeerAddress pa);

    // Announce to every node that has returned a token.  Prefer
    // register_torrent() for long-lived seeding sessions.
    void announce(const InfoHash& info_hash, uint16_t port);

    // Read-only view of our node ID (useful for logging / integration tests).
    const NodeId& self_id() const
    {
        return self_id_;
    }

    // Number of nodes currently in the routing table.
    size_t routing_table_size() const;

    // Shared DHT file logger
    void set_dht_logger(std::shared_ptr<logger::Logger> logger);

   private:
    // ---- Network I/O -------------------------------------------------------

    void send_krpc(const std::string& msg, PeerAddress pa);

    void log_krpc(const char* direction,
                  const std::string& msg,
                  PeerAddress pa) const;

    void log_krpc(const char* direction,
                  const KrpcMessage& msg,
                  PeerAddress pa) const;

    // Background thread: receive datagrams and dispatch them.
    void recv_loop();

    // Dispatch a parsed KRPC message.
    void handle_message(const KrpcMessage& msg, PeerAddress pa);

    // Handlers for each KRPC type.
    void on_ping(const KrpcMessage& msg, PeerAddress pa);
    void on_find_node(const KrpcMessage& msg, PeerAddress pa);
    void on_get_peers(const KrpcMessage& msg, PeerAddress pa);
    void on_announce_peer(const KrpcMessage& msg, PeerAddress pa);
    void on_response(const KrpcMessage& msg, PeerAddress pa);

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

    // Expire stale get_peers txns and advance active lookups (recv tick).
    void tick_get_peers_lookups();

    // Periodic get_peers + announce_peer for registered torrents.
    void maintain_registered_torrents();

    struct OutboundKrpc
    {
        std::string msg;
        PeerAddress pa;
    };

    void finish_lookup(const InfoHash& info_hash);

    // True when every known candidate was queried, nothing is in flight, and
    // merging the current routing-table closest set does not add new work.
    bool lookup_should_finish(KademliaLookup& lookup, const InfoHash& info_hash);

    // Send up to alpha parallel get_peers queries for an active lookup.
    std::vector<OutboundKrpc> advance_lookup(const InfoHash& info_hash);

    void on_lookup_response(const KrpcMessage& msg, const InfoHash& info_hash);

    void send_announce_requests(const std::vector<AnnounceRequest>& requests);

    void log_dht_info(const std::string& message) const;
    void log_dht_debug(const std::string& message) const;

    // ---- State -------------------------------------------------------------

    NodeId self_id_;           // this node's identity in the Kademlia keyspace
    uint16_t requested_port_;  // port passed to the constructor (0 = ephemeral)
    uint16_t port_;  // actual port currently bound (filled in by start())
    std::unique_ptr<UDPSocket> socket_;

    RoutingTable routing_table_;

    DhtPeerStore peer_store_;
    TokenSecretRotator token_rotator_;
    AnnounceCoordinator announce_coordinator_;
    // KRPC transaction id -> info_hash for in-flight get_peers lookups.
    std::unordered_map<
        std::string,
        std::pair<InfoHash, std::chrono::steady_clock::time_point>>
        pending_lookups_;

    // One iterative Kademlia lookup per info hash.
    std::unordered_map<InfoHash, KademliaLookup> kademlia_lookups_;

    static constexpr size_t LOOKUP_ALPHA = 6;

    mutable std::mutex peers_mutex_;

    std::atomic<bool> running_{false};

    // Wakes maintenance_loop() from wait_for() when stop() runs, so join() does
    // not block for the full maintenance interval (60s).
    std::mutex stop_mutex_;
    std::condition_variable stop_cv_;

    std::thread recv_thread_;
    std::thread maint_thread_;

    std::chrono::steady_clock::time_point last_bootstrap_attempt_{};
    std::chrono::steady_clock::time_point last_lookup_tick_{};
    bool bootstrap_connectivity_warned_{false};
    bool bootstrap_on_start_{true};

    // Round-robin offset into buckets_needing_refresh(); spreads refresh load.
    size_t refresh_bucket_cursor_{0};
    static constexpr auto BOOTSTRAP_RETRY_INTERVAL = std::chrono::minutes(2);

    PeerCallback peer_cb_;
    mutable std::mutex callback_mutex_;

    void invoke_peer_callback(const std::string& info_hash_hex, PeerAddress pa);

    // Protects dht_logger_ assignment; log calls rely on Logger::m_log_mutex.
    mutable std::mutex dht_logger_mutex_;
    std::shared_ptr<logger::Logger> dht_logger_;
};

}  // namespace dht
