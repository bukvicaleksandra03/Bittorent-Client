#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "peer.h"
#include "peer_wire/disk_writer.h"
#include "peer_wire/piece_manager.h"
#include "torrent_file.h"
#include "trackers/tracker_communicator.h"
#include "utils.h"

class TorrentManager
{
   public:
    TorrentManager(std::unique_ptr<TorrentFile> torrent,
                   const std::string& output_dir,
                   uint16_t listen_port = 6881);

    // Announces "started", preallocates output files, and spawns peer workers.
    void start();

    // Waits for spawned peer workers to finish.
    void stop();

    bool is_complete() const;
    double progress() const;

    const std::string& torrent_name() const;

    // Peer pool statistics (each worker tries one tracker peer index once).
    uint64_t peer_workers_started() const;
    uint64_t peer_handshakes_ok() const;
    uint64_t peer_handshake_failed() const;
    uint64_t peer_run_failed() const;

   private:
    enum class TrackerEvent : uint32_t
    {
        None = 0,
        Completed = 1,
        Started = 2,
        Stopped = 3,
    };

    void announce(TrackerEvent event);
    uint64_t downloaded_bytes() const;

    void run_peer_worker(size_t peer_index);
    void on_peer_worker_finished();
    void spawn_peers_locked();

    std::unique_ptr<TorrentFile> m_torrent;
    utils::PeerId m_peer_id{};
    uint16_t m_port = 6881;

    DiskWriter m_disk_writer;
    PieceManager m_piece_manager;

    std::unique_ptr<TrackerCommunicator> m_tracker;

    // Full tracker peer list; workers pull the next index until exhausted.
    std::vector<Peer> m_all_peers;
    size_t m_next_peer_index = 0;
    size_t m_workers_running = 0;
    static constexpr size_t k_max_concurrent_peers = 30;

    std::mutex m_spawn_mu;
    std::vector<std::thread> m_threads;

    std::atomic<uint64_t> m_peer_workers_started{0};
    std::atomic<uint64_t> m_peer_handshakes_ok{0};
    std::atomic<uint64_t> m_peer_handshake_failed{0};
    std::atomic<uint64_t> m_peer_run_failed{0};

    std::atomic<bool> m_started{false};
    std::atomic<bool> m_stopped{false};
};
