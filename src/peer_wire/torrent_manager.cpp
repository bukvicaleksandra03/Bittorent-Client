#include "peer_wire/torrent_manager.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <utility>

#include "peer_wire/peer_connection.h"
#include "trackers/tracker_communicator_factory.h"

namespace
{

// Replace filesystem-unfriendly characters so the torrent name can be used
// safely as part of a log file path on any OS.
std::string sanitize_filename(const std::string& name)
{
    std::string out;
    out.reserve(name.size());
    for (unsigned char c : name)
    {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.')
        {
            out += static_cast<char>(c);
        }
        else
        {
            out += '_';
        }
    }
    return out;
}

}  // namespace

TorrentManager::TorrentManager(std::unique_ptr<TorrentFile> torrent,
                               const std::string& output_dir,
                               const std::string& log_output_dir,
                               logger::Level log_level,
                               uint16_t listen_port)
    : m_torrent(std::move(torrent)),
      m_output_dir(output_dir),
      m_log_output_dir(log_output_dir),
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
        throw std::runtime_error("TorrentManager received null TorrentFile");
    }

    // Per-torrent logger – one log file, one mutex, shared by all
    // TorrentManager methods. Peer threads get their own loggers below.
    m_logger = std::make_shared<logger::Logger>();
    m_logger->set_level(log_level);
    // Clear any logs from a previous run so each run starts with a clean slate.
    fs::path log_dir =
        fs::path(log_output_dir) / sanitize_filename(m_torrent->get_name());
    if (fs::exists(log_dir))
    {
        fs::remove_all(log_dir);
    }
    fs::create_directories(log_dir);
    m_logger->set_file(
        (log_dir / (sanitize_filename(m_torrent->get_name()) + ".log"))
            .string());

    m_piece_manager.set_logger(m_logger);
}

const std::string& TorrentManager::log_output_dir() const
{
    return m_log_output_dir;
}

logger::Level TorrentManager::log_level() const
{
    return m_logger->get_level();
}

void TorrentManager::start()
{
    if (m_started.exchange(true))
    {
        LLOG_WARNING(*m_logger, "start() called more than once, ignoring");
        return;
    }

    m_peer_id = utils::generate_peer_id();
    m_tracker =
        create_communicator(m_torrent->get_tracker().protocol, m_logger);

    m_disk_writer.preallocate_files();
    announce(TrackerEvent::Started);

    std::vector<PeerAddress> peers =
        m_tracker->announce(m_torrent->get_tracker(),
                            m_torrent->get_info_hash(),
                            m_peer_id,
                            downloaded_bytes(),
                            m_torrent->get_total_size() - downloaded_bytes(),
                            0,
                            static_cast<uint32_t>(TrackerEvent::None),
                            m_port);

    if (peers.empty())
    {
        LLOG_WARNING(*m_logger, "tracker returned no peers");
        return;
    }
    else
    {
        LLOG_INFO(
            *m_logger,
            "tracker returned " + std::to_string(peers.size()) + " peer(s):");
        for (size_t i = 0; i < peers.size(); ++i)
        {
            LLOG_INFO(*m_logger,
                      "  [" + std::to_string(i) + "] " + peers[i].to_string());
        }
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

    const PeerAddress& peer = m_all_peers[peer_index];

    // Each PeerConnection gets its own Logger with its own mutex and its own
    // log file. Peer threads therefore never contend on a shared log lock,
    // eliminating the logging bottleneck at high peer counts.
    auto peer_logger = std::make_shared<logger::Logger>();
    peer_logger->set_level(m_logger->get_level());
    peer_logger->set_prefix("[" + peer.to_string() + "] ");
    peer_logger->set_file(
        (fs::path(m_log_output_dir) / sanitize_filename(m_torrent->get_name()) /
         ("peer_" + peer.ip + "_" + std::to_string(peer.port) + ".log"))
            .string());

    try
    {
        PeerConnection conn(peer,
                            m_torrent->get_info_hash(),
                            m_peer_id,
                            m_piece_manager,
                            peer_logger);

        // Register BEFORE connect() so that stop() can call cancel() on this
        // connection even when it is blocked inside do_handshake() waiting for
        // the peer's BitTorrent handshake reply.  The socket fd is created by
        // TCPClientSocket's constructor, so shutdown(fd, SHUT_RDWR) is valid
        // from this point onward.
        {
            std::lock_guard<std::mutex> lock(m_spawn_mu);
            m_active_connections.push_back(&conn);
        }

        auto unregister = [&]()
        {
            std::lock_guard<std::mutex> lock(m_spawn_mu);
            // Fold this connection's upload totals into the retired counters
            // before removing it, so torrent-wide totals survive peer churn.
            m_uploaded_bytes_retired.fetch_add(conn.bytes_uploaded(),
                                               std::memory_order_relaxed);
            m_blocks_uploaded_retired.fetch_add(conn.blocks_uploaded(),
                                                std::memory_order_relaxed);
            auto& v = m_active_connections;
            v.erase(std::remove(v.begin(), v.end(), &conn), v.end());
        };

        try
        {
            conn.connect();
        }
        catch (const std::exception& e)
        {
            unregister();
            m_peer_handshake_failed.fetch_add(1, std::memory_order_relaxed);
            LLOG_WARNING(*m_logger,
                         "peer " + peer.to_string() +
                             " handshake/connect failed: " + e.what());
            on_peer_worker_finished();
            return;
        }

        m_peer_handshakes_ok.fetch_add(1, std::memory_order_relaxed);

        if (m_stopped.load(std::memory_order_relaxed))
        {
            unregister();
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
            LLOG_WARNING(
                *m_logger,
                "peer " + peer.to_string() + " run failed: " + e.what());
        }

        // Unregister regardless of how run() exited.
        unregister();
    }
    catch (const std::exception& e)
    {
        m_peer_handshake_failed.fetch_add(1, std::memory_order_relaxed);
        LLOG_WARNING(*m_logger,
                     "peer worker " + std::to_string(peer_index) +
                         " failed: " + e.what());
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

    // Unblock every peer thread that is currently blocked inside recv().
    // shutdown() makes the blocked recv() return immediately with an error,
    // which causes run() to throw and the thread to exit.
    {
        std::lock_guard<std::mutex> lock(m_spawn_mu);
        for (PeerConnection* conn : m_active_connections)
        {
            conn->cancel();
        }
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

uint64_t TorrentManager::downloaded_bytes() const
{
    return m_piece_manager.downloaded_bytes();
}

uint64_t TorrentManager::total_bytes() const
{
    return m_torrent->get_total_size();
}

std::vector<TorrentManager::PeerStat> TorrentManager::top_peers(size_t n) const
{
    std::vector<PeerStat> stats;
    {
        std::lock_guard<std::mutex> lock(m_spawn_mu);
        stats.reserve(m_active_connections.size());
        for (const PeerConnection* conn : m_active_connections)
        {
            stats.push_back({conn->peer_address(), conn->bytes_downloaded()});
        }
    }
    std::sort(stats.begin(),
              stats.end(),
              [](const PeerStat& a, const PeerStat& b)
              { return a.bytes > b.bytes; });
    if (stats.size() > n)
    {
        stats.resize(n);
    }
    return stats;
}

std::vector<TorrentManager::PeerStat> TorrentManager::top_upload_peers(
    size_t n) const
{
    std::vector<PeerStat> stats;
    {
        std::lock_guard<std::mutex> lock(m_spawn_mu);
        stats.reserve(m_active_connections.size());
        for (const PeerConnection* conn : m_active_connections)
        {
            const uint64_t up = conn->bytes_uploaded();
            // Only report peers we are actually seeding to.
            if (up > 0)
            {
                stats.push_back({conn->peer_address(), up});
            }
        }
    }
    std::sort(stats.begin(),
              stats.end(),
              [](const PeerStat& a, const PeerStat& b)
              { return a.bytes > b.bytes; });
    if (stats.size() > n)
    {
        stats.resize(n);
    }
    return stats;
}

uint64_t TorrentManager::uploaded_bytes() const
{
    std::lock_guard<std::mutex> lock(m_spawn_mu);
    uint64_t total = m_uploaded_bytes_retired.load(std::memory_order_relaxed);
    for (const PeerConnection* conn : m_active_connections)
    {
        total += conn->bytes_uploaded();
    }
    return total;
}

uint64_t TorrentManager::blocks_uploaded() const
{
    std::lock_guard<std::mutex> lock(m_spawn_mu);
    uint64_t total = m_blocks_uploaded_retired.load(std::memory_order_relaxed);
    for (const PeerConnection* conn : m_active_connections)
    {
        total += conn->blocks_uploaded();
    }
    return total;
}

bool TorrentManager::is_seeding() const
{
    return is_complete() && !m_stopped.load(std::memory_order_relaxed);
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

    const uint64_t downloaded = m_piece_manager.downloaded_bytes();
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
        LLOG_WARNING(*m_logger,
                     "tracker announce failed (event=" +
                         std::to_string(static_cast<uint32_t>(event)) +
                         "): " + e.what());
    }
}

const std::string& TorrentManager::torrent_name() const
{
    return m_torrent->get_name();
}

void TorrentManager::add_dht_peer(const PeerAddress& peer)
{
    std::lock_guard<std::mutex> lock(m_spawn_mu);
    // Skip duplicates already in the pool.
    for (const auto& p : m_all_peers)
    {
        if (p.ip == peer.ip && p.port == peer.port)
        {
            return;
        }
    }
    m_all_peers.push_back(peer);
    spawn_peers_locked();
}

std::string TorrentManager::info_hash_str() const
{
    const auto& h = m_torrent->get_info_hash();
    return std::string(reinterpret_cast<const char*>(h.data()), h.size());
}

std::string TorrentManager::info_hash_hex() const
{
    return m_torrent->get_info_hash_hex();
}
