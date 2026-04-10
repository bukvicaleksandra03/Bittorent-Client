#include "peer_wire/torrent_manager.h"

#include <algorithm>
#include <exception>

#include "logger.h"
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
                      m_torrent->get_piece_hashes())
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

    const auto peers =
        m_tracker->announce(m_torrent->get_tracker(), m_torrent->get_info_hash(),
                            m_peer_id, downloaded_bytes(),
                            m_torrent->get_total_size() - downloaded_bytes(), 0,
                            static_cast<uint32_t>(TrackerEvent::None), m_port);

    if (peers.empty())
    {
        LOG_W("Tracker returned no peers");
        return;
    }

    const size_t max_connections = 30;
    const size_t to_spawn = std::min(peers.size(), max_connections);
    m_connections.reserve(to_spawn);
    m_threads.reserve(to_spawn);

    for (size_t i = 0; i < to_spawn; ++i)
    {
        m_connections.emplace_back(std::make_unique<PeerConnection>(
            peers[i], m_torrent->get_info_hash(), m_peer_id, m_piece_manager));
        PeerConnection* conn = m_connections.back().get();

        m_threads.emplace_back([conn, i]() {
            try
            {
                conn->connect();
                conn->run();
            }
            catch (const std::exception& e)
            {
                LOG_W("Peer worker " + std::to_string(i) +
                      " terminated with error: " + std::string(e.what()));
            }
        });
    }
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
        // If download finished, report completion first, then stopped.
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
    const uint32_t total = m_piece_manager.num_pieces();
    if (total == 0)
    {
        return 0.0;
    }

    uint32_t done = 0;
    for (uint32_t i = 0; i < total; ++i)
    {
        if (m_piece_manager.have_piece(i))
        {
            ++done;
        }
    }

    return static_cast<double>(done) / static_cast<double>(total);
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
        (void)m_tracker->announce(m_torrent->get_tracker(), m_torrent->get_info_hash(),
                                  m_peer_id, downloaded, left, 0,
                                  static_cast<uint32_t>(event), m_port);
    }
    catch (const std::exception& e)
    {
        LOG_W("Tracker announce failed (" + std::to_string(static_cast<uint32_t>(event)) +
              "): " + e.what());
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
