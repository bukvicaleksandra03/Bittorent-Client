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
//   node.announce("info_hash_20_bytes", 6881);  // after connect
//   ...
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

    // Gracefully stop the background threads and close the socket.
    void stop();

    bool is_running() const
    {
        return running_.load();
    }

    // Register a callback that fires when peers are found for a torrent.
    void set_peer_callback(PeerCallback cb)
    {
        peer_cb_ = std::move(cb);
    }

    // Start (or refresh) a get_peers lookup for the given 20-byte info hash.
    void get_peers(const std::string& info_hash_20);

    // Announce that we are a peer for the given info hash on the given port.
    // Typically called after a get_peers lookup has returned tokens.
    void announce(const std::string& info_hash_20, uint16_t port);

    // Read-only view of our node ID (useful for logging / integration tests).
    const NodeId& self_id() const
    {
        return self_id_;
    }

    // Number of nodes currently in the routing table.
    size_t routing_table_size() const;

    // Register a file logger for KRPC traffic.  Each torrent may add its own
    // logger; log_krpc() fans out to every registered logger.
    void add_krpc_logger(std::shared_ptr<logger::Logger> logger);

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

    NodeId self_id_;
    uint16_t requested_port_;  // port passed to the constructor (0 = ephemeral)
    uint16_t port_;  // actual port currently bound (filled in by start())
    std::unique_ptr<UDPSocket> socket_;

    mutable std::mutex table_mutex_;
    RoutingTable routing_table_;

    // info_hash (20 bytes) -> list of compact peer strings (6 bytes each)
    // that we know about but haven't yet announced.
    std::unordered_map<std::string, std::vector<std::string>> peer_store_;
    // tokens returned by remote nodes keyed by "ip:port"
    std::unordered_map<std::string, std::string> received_tokens_;
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

    PeerCallback peer_cb_;

    mutable std::mutex krpc_loggers_mutex_;
    std::vector<std::shared_ptr<logger::Logger>> krpc_loggers_;
};

}  // namespace dht
