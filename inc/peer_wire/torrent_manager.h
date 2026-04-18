#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "peer_wire/disk_writer.h"
#include "peer_wire/peer_connection.h"
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

    std::unique_ptr<TorrentFile> m_torrent;
    utils::PeerId m_peer_id{};
    uint16_t m_port = 6881;

    DiskWriter m_disk_writer;
    PieceManager m_piece_manager;

    std::unique_ptr<TrackerCommunicator> m_tracker;
    std::vector<std::unique_ptr<PeerConnection>> m_connections;
    std::vector<std::thread> m_threads;
    std::atomic<bool> m_started{false};
    std::atomic<bool> m_stopped{false};
};
