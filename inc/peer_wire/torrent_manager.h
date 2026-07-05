#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "logger.h"
#include "peer.h"
#include "peer_wire/disk_writer.h"
#include "peer_wire/piece_manager.h"
#include "torrent_file.h"
#include "trackers/tracker_communicator.h"
#include "utils.h"

class PeerConnection;  // forward declaration — avoids a circular include

class TorrentManager
{
   public:
    TorrentManager(std::unique_ptr<TorrentFile> torrent,
                   const std::string& output_dir,
                   const std::string& log_output_dir,
                   logger::Level log_level = logger::Level::DEBUG,
                   uint16_t listen_port = 6881);

    // Announces "started", preallocates output files, and spawns peer workers.
    void start();

    // Waits for spawned peer workers to finish.
    void stop();

    bool is_complete() const;
    double progress() const;

    // Bytes successfully downloaded and verified so far.
    uint64_t downloaded_bytes() const;

    // Total size of the torrent in bytes.
    uint64_t total_bytes() const;

    // Total payload bytes served to peers across the torrent's lifetime
    // (sum of currently-active connections plus connections that have closed).
    uint64_t uploaded_bytes() const;

    // Total number of blocks (Piece messages) served to peers across the
    // torrent's lifetime.
    uint64_t blocks_uploaded() const;

    // True once the download is complete and the torrent is actively seeding
    // (i.e. complete and not stopped).
    bool is_seeding() const;

    const std::string& torrent_name() const;

    // Peer pool statistics (each worker tries one tracker peer index once).
    uint64_t peer_workers_started() const;
    uint64_t peer_handshakes_ok() const;
    uint64_t peer_handshake_failed() const;
    uint64_t peer_run_failed() const;

    // Per-peer download statistics returned by top_peers().
    struct PeerStat
    {
        std::string address;  // "ip:port"
        uint64_t bytes;       // total payload bytes received from this peer
    };

    // Snapshot the bytes downloaded from every active peer connection, sort
    // descending, and return the top `n` entries.  Thread-safe.
    std::vector<PeerStat> top_peers(size_t n) const;

    // Snapshot the bytes uploaded to every active peer connection, keep only
    // peers we have actually served data to, sort descending, and return the
    // top `n` entries.  Thread-safe.
    std::vector<PeerStat> top_upload_peers(size_t n) const;

   private:
    enum class TrackerEvent : uint32_t
    {
        None = 0,
        Completed = 1,
        Started = 2,
        Stopped = 3,
    };

    void announce(TrackerEvent event);

    void run_peer_worker(size_t peer_index);
    void on_peer_worker_finished();
    void spawn_peers_locked();

    std::unique_ptr<TorrentFile> m_torrent;
    std::string m_output_dir;
    std::string m_log_output_dir;
    utils::PeerId m_peer_id{};
    uint16_t m_port = 6881;

    // Per-torrent logger: writes to <log_output_dir>/<torrent_name>.log.
    // Per-peer loggers are created in run_peer_worker and passed to each
    // PeerConnection, so peer threads never share a log mutex.
    std::shared_ptr<logger::Logger> m_logger;

    DiskWriter m_disk_writer;
    PieceManager m_piece_manager;

    std::unique_ptr<TrackerCommunicator> m_tracker;

    // Full tracker peer list; workers pull the next index until exhausted.
    std::vector<Peer> m_all_peers;
    size_t m_next_peer_index = 0;
    size_t m_workers_running = 0;
    static constexpr size_t k_max_concurrent_peers = 10;

    mutable std::mutex m_spawn_mu;
    std::vector<std::thread> m_threads;

    // Connections that are currently inside PeerConnection::run().
    // Written under m_spawn_mu; read by stop() before joining threads.
    std::vector<PeerConnection*> m_active_connections;

    // Upload stats from connections that have already closed.  Live
    // connections are summed on demand; these accumulate the rest so totals
    // survive peer churn.  Updated under m_spawn_mu in run_peer_worker.
    std::atomic<uint64_t> m_uploaded_bytes_retired{0};
    std::atomic<uint64_t> m_blocks_uploaded_retired{0};

    std::atomic<uint64_t> m_peer_workers_started{0};
    std::atomic<uint64_t> m_peer_handshakes_ok{0};
    std::atomic<uint64_t> m_peer_handshake_failed{0};
    std::atomic<uint64_t> m_peer_run_failed{0};

    std::atomic<bool> m_started{false};
    std::atomic<bool> m_stopped{false};
};
