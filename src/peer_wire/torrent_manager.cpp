#include "peer_wire/torrent_manager.h"

#include <exception>
#include <utility>

#include "logger.h"
#include "peer_wire/peer_connection.h"
#include "trackers/tracker_communicator_factory.h"

TorrentManager::TorrentManager(std::unique_ptr<TorrentFile> torrent,
                               const std::string& output_dir,
                               uint16_t listen_port)
    : m_torrent(std::move(torrent)),
      m_port(listen_port),
      m_disk_writer(*m_torrent, output_dir),
      m_piece_manager(static_cast<uint32_t>(m_torrent->get_piece_count()),
                      static_cast<uint32_t>(m_torrent->get_piece_size()),
                      m_torrent->get_total_size(),
                      m_torrent->get_piece_hashes(),
                      m_disk_writer)
{
    if (!m_torrent)
    {
        LOG_AND_THROW("TorrentManager received null TorrentFile");
    }
}

void TorrentManager::start()
{
    if (m_started.exchange(true))
    {
        LOG_W("TorrentManager::start called more than once, ignoring");
        return;
    }

    m_peer_id = utils::generate_peer_id();
    m_tracker = create_communicator(m_torrent->get_tracker().protocol);

    m_disk_writer.preallocate_files();
    announce(TrackerEvent::Started);

    std::vector<Peer> peers = m_tracker->announce(
        m_torrent->get_tracker(),
        m_torrent->get_info_hash(),
        m_peer_id,
        downloaded_bytes(),
        m_torrent->get_total_size() - downloaded_bytes(),
        0,
        static_cast<uint32_t>(TrackerEvent::None),
        m_port);

    if (peers.empty())
    {
        LOG_W("Tracker returned no peers");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_spawn_mu);
        m_all_peers = std::move(peers);
        m_next_peer_index = 0;
        m_workers_running = 0;
        spawn_peers_locked();
    }
}

void TorrentManager::spawn_peers_locked()
{
    while (!m_stopped.load(std::memory_order_relaxed) &&
           m_workers_running < k_max_concurrent_peers &&
           m_next_peer_index < m_all_peers.size())
    {
        if (m_piece_manager.is_complete())
        {
            break;
        }

        const size_t idx = m_next_peer_index++;
        ++m_workers_running;

        m_threads.emplace_back([this, idx]() { run_peer_worker(idx); });
    }
}

void TorrentManager::run_peer_worker(size_t peer_index)
{
    m_peer_workers_started.fetch_add(1, std::memory_order_relaxed);

    const Peer& peer = m_all_peers[peer_index];

    try
    {
        PeerConnection conn(peer,
                            m_torrent->get_info_hash(),
                            m_peer_id,
                            m_piece_manager);

        try
        {
            conn.connect();
        }
        catch (const std::exception& e)
        {
            m_peer_handshake_failed.fetch_add(1, std::memory_order_relaxed);
            LOG_W("Peer " + peer.to_string() + " handshake/connect failed: " +
                  std::string(e.what()));
            on_peer_worker_finished();
            return;
        }

        m_peer_handshakes_ok.fetch_add(1, std::memory_order_relaxed);

        if (m_stopped.load(std::memory_order_relaxed))
        {
            on_peer_worker_finished();
            return;
        }

        try
        {
            conn.run();
        }
        catch (const std::exception& e)
        {
            m_peer_run_failed.fetch_add(1, std::memory_order_relaxed);
            LOG_W("Peer " + peer.to_string() + " run failed: " +
                  std::string(e.what()));
        }
    }
    catch (const std::exception& e)
    {
        m_peer_handshake_failed.fetch_add(1, std::memory_order_relaxed);
        LOG_W("Peer worker " + std::to_string(peer_index) + " failed: " +
              std::string(e.what()));
    }

    on_peer_worker_finished();
}

void TorrentManager::on_peer_worker_finished()
{
    std::lock_guard<std::mutex> lock(m_spawn_mu);
    if (m_workers_running > 0)
    {
        --m_workers_running;
    }
    spawn_peers_locked();
}

void TorrentManager::stop()
{
    if (m_stopped.exchange(true))
    {
        return;
    }

    for (auto& th : m_threads)
    {
        if (th.joinable())
        {
            th.join();
        }
    }

    if (m_tracker)
    {
        if (is_complete())
        {
            announce(TrackerEvent::Completed);
        }
        announce(TrackerEvent::Stopped);
    }
}

bool TorrentManager::is_complete() const
{
    return m_piece_manager.is_complete();
}

double TorrentManager::progress() const
{
    return m_piece_manager.percentage_complete();
}

uint64_t TorrentManager::peer_workers_started() const
{
    return m_peer_workers_started.load(std::memory_order_relaxed);
}

uint64_t TorrentManager::peer_handshakes_ok() const
{
    return m_peer_handshakes_ok.load(std::memory_order_relaxed);
}

uint64_t TorrentManager::peer_handshake_failed() const
{
    return m_peer_handshake_failed.load(std::memory_order_relaxed);
}

uint64_t TorrentManager::peer_run_failed() const
{
    return m_peer_run_failed.load(std::memory_order_relaxed);
}

void TorrentManager::announce(TrackerEvent event)
{
    if (!m_tracker)
    {
        return;
    }

    const uint64_t downloaded = downloaded_bytes();
    const uint64_t total = m_torrent->get_total_size();
    const uint64_t left = (downloaded <= total) ? (total - downloaded) : 0;

    try
    {
        (void)m_tracker->announce(m_torrent->get_tracker(),
                                  m_torrent->get_info_hash(),
                                  m_peer_id,
                                  downloaded,
                                  left,
                                  0,
                                  static_cast<uint32_t>(event),
                                  m_port);
    }
    catch (const std::exception& e)
    {
        LOG_W("Tracker announce failed (" +
              std::to_string(static_cast<uint32_t>(event)) + "): " + e.what());
    }
}

uint64_t TorrentManager::downloaded_bytes() const
{
    uint64_t downloaded = 0;
    const uint32_t total = m_piece_manager.num_pieces();
    for (uint32_t i = 0; i < total; ++i)
    {
        if (m_piece_manager.have_piece(i))
        {
            downloaded += m_piece_manager.piece_length(i);
        }
    }
    return downloaded;
}

const std::string& TorrentManager::torrent_name() const
{
    return m_torrent->get_name();
}
