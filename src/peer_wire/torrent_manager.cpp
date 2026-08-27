#include "peer_wire/torrent_manager.h"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <stdexcept>
#include <utility>

#include "peer_wire/peer_connection.h"
#include "trackers/tracker_communicator_factory.h"
#include "utils.h"

namespace fs = std::filesystem;

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
    fs::path log_dir = fs::path(log_output_dir) /
                       utils::sanitize_filename(m_torrent->get_name());
    if (fs::exists(log_dir))
    {
        fs::remove_all(log_dir);
    }
    fs::create_directories(log_dir);
    m_logger->set_file(
        (log_dir / (utils::sanitize_filename(m_torrent->get_name()) + ".log"))
            .string());

    m_piece_manager.set_logger(m_logger);
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

    m_start_steady = std::chrono::steady_clock::now();

    m_peer_id = utils::generate_peer_id();
    m_tracker =
        create_communicator(m_torrent->get_tracker().protocol, m_logger);

    m_disk_writer.preallocate_files();

    const std::vector<PeerAddress> tracker_peers =
        announce(TrackerEvent::Started);

    {
        std::lock_guard<std::mutex> lock(m_spawn_mu);
        if (!tracker_peers.empty())
        {
            LLOG_INFO(*m_logger,
                      "tracker returned " +
                          std::to_string(tracker_peers.size()) + " peer(s):");
            for (size_t i = 0; i < tracker_peers.size(); ++i)
            {
                LLOG_INFO(*m_logger,
                          "  [" + std::to_string(i) + "] " +
                              tracker_peers[i].to_string());
            }
            append_unique_peers(tracker_peers);
        }
        else if (m_all_peers.empty())
        {
            LLOG_WARNING(*m_logger,
                         "tracker returned no peers; continuing with DHT only");
        }

        if (!m_all_peers.empty())
        {
            spawn_peers_locked();
        }
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

    // Copy under lock: DHT callbacks may push_back on m_all_peers and reallocate
    // storage while this worker runs; a reference would be use-after-free.
    PeerAddress peer;
    bool missing = false;
    {
        std::lock_guard<std::mutex> lock(m_spawn_mu);
        if (peer_index >= m_all_peers.size())
        {
            missing = true;
        }
        else
        {
            peer = m_all_peers[peer_index];
        }
    }
    if (missing)
    {
        on_peer_worker_finished();
        return;
    }

    // Each PeerConnection gets its own Logger with its own mutex and its own
    // log file. Peer threads therefore never contend on a shared log lock,
    // eliminating the logging bottleneck at high peer counts.
    auto peer_logger = std::make_shared<logger::Logger>();
    peer_logger->set_level(m_logger->get_level());
    peer_logger->set_prefix("[" + peer.to_string() + "] ");
    peer_logger->set_file(
        (fs::path(m_log_output_dir) /
         utils::sanitize_filename(m_torrent->get_name()) /
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

uint64_t TorrentManager::piece_hash_failures() const
{
    return m_piece_manager.piece_hash_failures();
}

std::vector<PeerAddress> TorrentManager::announce(TrackerEvent event)
{
    if (!m_tracker)
    {
        return {};
    }

    const uint64_t downloaded = m_piece_manager.downloaded_bytes();
    const uint64_t total = m_torrent->get_total_size();
    const uint64_t left = (downloaded <= total) ? (total - downloaded) : 0;

    try
    {
        return m_tracker->announce(m_torrent->get_tracker(),
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
        return {};
    }
}

void TorrentManager::append_unique_peers(const std::vector<PeerAddress>& peers)
{
    for (const PeerAddress& peer : peers)
    {
        const auto it = std::find(m_all_peers.begin(), m_all_peers.end(), peer);
        if (it == m_all_peers.end())
        {
            m_all_peers.push_back(peer);
        }
    }
}

const std::string& TorrentManager::torrent_name() const
{
    return m_torrent->get_name();
}

size_t TorrentManager::dht_peers_discovered() const
{
    return m_dht_peers_discovered.load();
}

bool TorrentManager::add_dht_peer(const PeerAddress& peer)
{
    std::lock_guard<std::mutex> lock(m_spawn_mu);
    const size_t before = m_all_peers.size();
    append_unique_peers({peer});
    if (m_all_peers.size() == before)
    {
        return false;
    }
    const size_t total = m_dht_peers_discovered.fetch_add(1) + 1;
    LLOG_INFO(*m_logger,
              "DHT discovered new peer: " + peer.to_string() +
                  " (total DHT peers: " + std::to_string(total) + ")");
    spawn_peers_locked();
    return true;
}

const InfoHash& TorrentManager::info_hash() const
{
    return m_torrent->get_info_hash();
}

std::string TorrentManager::info_hash_str() const
{
    return m_torrent->get_info_hash().to_raw();
}

std::string TorrentManager::info_hash_hex() const
{
    return m_torrent->get_info_hash_hex();
}

void TorrentManager::record_download_sample(double download_bps)
{
    if (download_bps > m_peak_speed_bps)
    {
        m_peak_speed_bps = download_bps;
    }
}

void TorrentManager::mark_complete_at(
    std::chrono::steady_clock::time_point when)
{
    if (m_complete_steady == std::chrono::steady_clock::time_point{})
    {
        m_complete_steady = when;
    }
}

TorrentRunSummary TorrentManager::build_run_summary() const
{
    TorrentRunSummary summary;
    summary.torrent_name = torrent_name();
    summary.info_hash_hex = info_hash_hex();
    summary.size = utils::format_byte_size(total_bytes());
    summary.complete = is_complete();
    summary.dht_peers_discovered = dht_peers_discovered();
    summary.peer_workers_started = peer_workers_started();
    summary.peer_handshakes_ok = peer_handshakes_ok();
    summary.peer_handshake_failed = peer_handshake_failed();
    summary.peer_run_failed = peer_run_failed();
    summary.piece_hash_failures = piece_hash_failures();
    summary.peak_speed_bps = m_peak_speed_bps;

    const auto start = m_start_steady;
    auto end = std::chrono::steady_clock::now();
    if (m_complete_steady != std::chrono::steady_clock::time_point{})
    {
        end = m_complete_steady;
    }

    double duration_sec = 0.0;
    if (start != std::chrono::steady_clock::time_point{})
    {
        duration_sec = std::chrono::duration<double>(end - start).count();
    }
    summary.duration = utils::format_duration(duration_sec);

    if (duration_sec > 0.0)
    {
        summary.avg_speed_bps =
            static_cast<double>(downloaded_bytes()) / duration_sec;
    }

    return summary;
}

bool TorrentManager::try_mark_summary_written() const
{
    bool expected = false;
    return m_summary_written.compare_exchange_strong(
        expected, true, std::memory_order_relaxed);
}

void TorrentManager::write_run_summary_json(
    const TorrentRunSummary& summary,
    const std::string& output_path,
    std::optional<bool> reference_match)
{
    fs::create_directories(fs::path(output_path).parent_path());

    std::ofstream out(output_path);
    if (!out)
    {
        return;
    }

    out << std::fixed << std::setprecision(2);
    out << "{\n";
    out << "  \"torrent_name\": \"" << utils::json_escape(summary.torrent_name)
        << "\",\n";
    out << "  \"info_hash_hex\": \"" << utils::json_escape(summary.info_hash_hex)
        << "\",\n";
    out << "  \"duration\": \"" << utils::json_escape(summary.duration)
        << "\",\n";
    out << "  \"size\": \"" << utils::json_escape(summary.size) << "\",\n";
    out << "  \"avg_speed\": \""
        << utils::json_escape(utils::format_byte_rate(summary.avg_speed_bps))
        << "\",\n";
    out << "  \"peak_speed\": \""
        << utils::json_escape(utils::format_byte_rate(summary.peak_speed_bps))
        << "\",\n";
    out << std::setprecision(0);
    out << "  \"dht_peers_discovered\": " << summary.dht_peers_discovered
        << ",\n";
    out << "  \"peer_workers_started\": " << summary.peer_workers_started
        << ",\n";
    out << "  \"peer_handshakes_ok\": " << summary.peer_handshakes_ok << ",\n";
    out << "  \"peer_handshake_failed\": " << summary.peer_handshake_failed
        << ",\n";
    out << "  \"peer_run_failed\": " << summary.peer_run_failed << ",\n";
    out << "  \"piece_hash_failures\": " << summary.piece_hash_failures
        << ",\n";
    out << "  \"complete\": " << (summary.complete ? "true" : "false");
    if (reference_match.has_value())
    {
        out << ",\n  \"reference_match\": "
            << (reference_match.value() ? "true" : "false");
    }
    out << "\n}\n";
}

void TorrentManager::write_run_summary_json(
    const std::string& output_path,
    std::optional<bool> reference_match) const
{
    write_run_summary_json(build_run_summary(), output_path, reference_match);
}
