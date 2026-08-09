#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "logger.h"
#include "net/upnp.h"

namespace dht
{
class DhtClient;
}

class TorrentManager;

// Owns multiple TorrentManager instances, coordinates start/stop, and prints
// human-readable status to a stream (separate from structured file logging).
// Optional background thread clears the terminal and redraws status on an
// interval; call stop_status_refresh() before destroying the ostream passed to
// start_status_refresh (typically join stop_all() which stops the refresh).
class SessionManager
{
   public:
    // listen_port is the single TCP port used by *all* torrents in this
    // session. A single UPnP mapping is attempted at startup and kept alive for
    // the whole session lifetime; released when the SessionManager is
    // destroyed.
    explicit SessionManager(uint16_t listen_port = 6881);

    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    ~SessionManager();

    void add(std::unique_ptr<TorrentManager> manager);

    void start_all();
    void stop_all();

    size_t session_count() const;

    bool all_complete() const;

    // One line per session: name, progress %, complete flag.
    void print_status(std::ostream& os) const;

    // Spawns a thread that clears the screen and prints status every
    // `interval`. Idempotent if already running. `os` must outlive
    // stop_status_refresh() (or stop_all(), which stops the refresh thread).
    void start_status_refresh(std::ostream& os,
                              std::chrono::milliseconds interval);

    void stop_status_refresh();

    bool status_refresh_running() const;

   private:
    void print_status_locked(std::ostream& os) const;
    void status_thread_main();
    void init_session_logger();

    mutable std::mutex m_mutex;
    std::vector<std::unique_ptr<TorrentManager>> m_sessions;

    std::thread m_status_thread;
    std::atomic<bool> m_status_stop{true};
    std::ostream* m_status_stream{nullptr};
    std::chrono::milliseconds m_status_interval{
        std::chrono::milliseconds{1000}};

    // Speed tracking — all guarded by m_mutex (mutable so print_status_locked
    // can update them in a const context).
    mutable std::vector<uint64_t> m_prev_bytes;
    mutable std::chrono::steady_clock::time_point m_last_snapshot;
    mutable std::vector<double> m_speeds_bps;

    // Upload (seeding) speed tracking, parallel to the download vectors above.
    mutable std::vector<uint64_t> m_prev_up_bytes;
    mutable std::vector<double> m_up_speeds_bps;

    // Single UPnP port mapping shared by all torrents in the session.
    // Kept alive until the SessionManager is destroyed.
    uint16_t m_listen_port{6881};
    std::optional<UPnPPortMapping> m_upnp_mapping;

    // DHT client — one per session, shared across all torrents.
    std::unique_ptr<dht::DhtClient> m_dht_client;
    std::shared_ptr<logger::Logger> m_dht_logger;
    std::shared_ptr<logger::Logger> m_session_logger;
};
