#include "peer_wire/session_manager.h"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>

#include "dht/dht_client.h"
#include "peer_address.h"
#include "peer_wire/torrent_manager.h"
#include "utils.h"

namespace fs = std::filesystem;

namespace
{
// Clear screen and move cursor home (ANSI). Works in most modern terminals.
constexpr const char k_clear_screen[] = "\033[2J\033[H";

std::string json_escape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s)
    {
        switch (c)
        {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out += c;
                break;
        }
    }
    return out;
}

}  // namespace

SessionManager::SessionManager(uint16_t listen_port)
    : m_listen_port(listen_port)
{
    init_session_logger();
}

void SessionManager::init_session_logger()
{
    if (m_session_logger)
    {
        return;
    }

    const std::string log_dir = "logs";
    fs::create_directories(log_dir);
    const fs::path log_path = fs::path(log_dir) / "session_manager.log";
    if (fs::exists(log_path))
    {
        fs::remove(log_path);
    }

    m_session_logger = std::make_shared<logger::Logger>();
    m_session_logger->set_level(logger::Level::INFO);
    m_session_logger->set_file(log_path.string());
}

SessionManager::~SessionManager()
{
    stop_status_refresh();
    stop_all();
}

void SessionManager::add(std::unique_ptr<TorrentManager> manager)
{
    if (!manager)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sessions.push_back(std::move(manager));
}

void SessionManager::start_all()
{
    init_session_logger();

    // Attempt a single UPnP port mapping for the shared listen port.
    // Best-effort: we continue even if it fails (outbound connections still
    // function without an open inbound mapping).
    if (!m_upnp_mapping)
    {
        m_upnp_mapping = UPnPPortMapping::create(m_listen_port);
        if (m_upnp_mapping)
        {
            LLOG_INFO(*m_session_logger,
                      "UPnP: mapped port " +
                          std::to_string(m_upnp_mapping->external_port()) +
                          " -> " + std::to_string(m_listen_port) +
                          " (external IP: " + m_upnp_mapping->external_ip() +
                          ")");
        }
        else
        {
            LLOG_INFO(*m_session_logger,
                      "UPnP: no gateway or mapping failed – continuing without "
                      "port mapping");
        }
    }

    // Start DHT node.  Use the same port as the BitTorrent listen port so the
    // UPnP mapping (if any) also covers DHT traffic.
    if (!m_dht_client)
    {
        m_dht_client = std::make_unique<dht::DhtClient>(m_listen_port);

        // When the DHT finds peers for an info hash, inject them into the
        // matching TorrentManager (if we have one).
        m_dht_client->set_peer_callback(
            [this](const std::string& info_hash_hex, PeerAddress pa)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                for (auto& s : m_sessions)
                {
                    if (!s || s->info_hash_hex() != info_hash_hex)
                    {
                        continue;
                    }
                    const bool added = s->add_dht_peer(pa);
                    LLOG_INFO(
                        *m_session_logger,
                        "DHT peer " + pa.to_string() + " routed to torrent \"" +
                            s->torrent_name() + "\" (" + info_hash_hex + ")" +
                            (added ? " (added)" : " (duplicate, not added)"));
                }
            });

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const fs::path dht_log_path = fs::path("logs") / "dht.log";
            if (fs::exists(dht_log_path))
            {
                fs::remove(dht_log_path);
            }

            logger::Level level = logger::Level::INFO;
            m_dht_logger = std::make_shared<logger::Logger>();
            m_dht_logger->set_level(level);
            m_dht_logger->set_file(dht_log_path.string());
            m_dht_client->set_dht_logger(m_dht_logger);
        }

        m_dht_client->start();
        LLOG_INFO(*m_session_logger,
                  "DHT: started on port " + std::to_string(m_listen_port) +
                      " (node id: " + m_dht_client->self_id().hex() + ")");
    }

    // Register active torrents for periodic DHT get_peers + announce_peer.
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_session_start_steady = std::chrono::steady_clock::now();
        m_session_start_system = std::chrono::system_clock::now();
        m_peak_speeds_bps.assign(m_sessions.size(), 0.0);
        m_metrics_csv_header_written.assign(m_sessions.size(), false);

        for (auto& s : m_sessions)
        {
            if (s)
            {
                m_dht_client->register_torrent(s->info_hash(), m_listen_port);
                s->start();
            }
        }
    }
}

void SessionManager::stop_all()
{
    stop_status_refresh();

    // Stop DHT before the torrent sessions so the callback cannot fire into
    // a half-destroyed TorrentManager.
    if (m_dht_client)
    {
        m_dht_client->stop();
        m_dht_client.reset();
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& s : m_sessions)
    {
        if (s)
        {
            s->stop();
        }
    }
}

size_t SessionManager::session_count() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_sessions.size();
}

bool SessionManager::all_complete() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& s : m_sessions)
    {
        if (!s || !s->is_complete())
        {
            return false;
        }
    }
    return !m_sessions.empty();
}

// Prints one block per session, for example:
//
//   ---- sessions (1) ----
//     ubuntu-22.04-desktop-amd64.iso           0.25%  (13.00 MiB / 5.20
//     GiB)  5.20 MiB/s  active
//     ---- top peers ----
//       192.168.1.1:6881              45.20 MiB
//       10.0.0.1:51413               32.10 MiB
//
void SessionManager::print_status_locked(std::ostream& os) const
{
    const auto now = std::chrono::steady_clock::now();

    // Grow parallel vectors to match the current number of sessions.
    m_prev_bytes.resize(m_sessions.size(), 0);
    m_speeds_bps.resize(m_sessions.size(), 0.0);
    m_prev_up_bytes.resize(m_sessions.size(), 0);
    m_up_speeds_bps.resize(m_sessions.size(), 0.0);

    if (m_last_snapshot == std::chrono::steady_clock::time_point{})
    {
        // First call: seed prev_bytes so the next interval gives a clean delta.
        for (size_t i = 0; i < m_sessions.size(); ++i)
        {
            if (m_sessions[i])
            {
                m_prev_bytes[i] = m_sessions[i]->downloaded_bytes();
                m_prev_up_bytes[i] = m_sessions[i]->uploaded_bytes();
            }
        }
        m_last_snapshot = now;
    }
    else
    {
        const double elapsed =
            std::chrono::duration<double>(now - m_last_snapshot).count();

        if (elapsed > 0.05)  // guard against near-zero division
        {
            for (size_t i = 0; i < m_sessions.size(); ++i)
            {
                if (!m_sessions[i])
                {
                    continue;
                }
                const uint64_t curr = m_sessions[i]->downloaded_bytes();
                const uint64_t delta =
                    (curr >= m_prev_bytes[i]) ? (curr - m_prev_bytes[i]) : 0;
                m_speeds_bps[i] = static_cast<double>(delta) / elapsed;
                m_prev_bytes[i] = curr;

                const uint64_t up_curr = m_sessions[i]->uploaded_bytes();
                const uint64_t up_delta = (up_curr >= m_prev_up_bytes[i])
                                              ? (up_curr - m_prev_up_bytes[i])
                                              : 0;
                m_up_speeds_bps[i] = static_cast<double>(up_delta) / elapsed;
                m_prev_up_bytes[i] = up_curr;

                if (i < m_peak_speeds_bps.size())
                {
                    m_peak_speeds_bps[i] =
                        std::max(m_peak_speeds_bps[i], m_speeds_bps[i]);
                }
            }
            m_last_snapshot = now;
        }
    }

    if (m_metrics_csv_header_written.size() < m_sessions.size())
    {
        m_metrics_csv_header_written.resize(m_sessions.size(), false);
    }
    for (size_t i = 0; i < m_sessions.size(); ++i)
    {
        if (m_sessions[i] && !m_metrics_dir.empty())
        {
            append_metrics_csv_locked(i);
        }
    }

    os << "---- sessions (" << m_sessions.size() << ") ----\n";
    for (size_t i = 0; i < m_sessions.size(); ++i)
    {
        const auto& s = m_sessions[i];
        if (!s)
        {
            continue;
        }

        const std::string size_info =
            "(" + utils::format_byte_size(s->downloaded_bytes()) + " / " +
            utils::format_byte_size(s->total_bytes()) + ")";

        const double speed = (i < m_speeds_bps.size()) ? m_speeds_bps[i] : 0.0;
        const std::string speed_str = utils::format_byte_rate(speed);

        const bool seeding = s->is_seeding();
        const char* state_str = seeding ? "seeding" : "active";

        os << "  " << std::setw(48) << std::left
           << s->torrent_name().substr(0, 48) << std::fixed
           << std::setprecision(2) << std::setw(7) << std::right
           << s->progress() << "%  " << std::setw(24) << std::left << size_info
           << "  " << std::setw(14) << std::left << speed_str << "  "
           << state_str << "\n";

        os << "    DHT peers discovered: " << s->dht_peers_discovered() << "\n";

        // While seeding, show what we have served: total uploaded, the number
        // of blocks/pieces served, and the current upload rate.
        if (seeding)
        {
            const double up_speed =
                (i < m_up_speeds_bps.size()) ? m_up_speeds_bps[i] : 0.0;
            const uint64_t up_bytes = s->uploaded_bytes();
            const uint64_t blocks = s->blocks_uploaded();
            const uint64_t total = s->total_bytes();
            // Approximate "pieces worth" of data served (16 KiB blocks make up
            // each piece, so this is uploaded / piece-equivalent of the whole
            // torrent — a rough, human-friendly figure).
            const std::string piece_note =
                (total > 0)
                    ? ("  (~" +
                       std::to_string(static_cast<uint64_t>(
                           up_bytes / static_cast<double>(total) * 100.0)) +
                       "% of torrent size)")
                    : "";

            os << "    seeded: " << utils::format_byte_size(up_bytes) << " in "
               << blocks << " blocks" << piece_note << "  up "
               << utils::format_byte_rate(up_speed) << "\n";
        }

        // Print top 10 peers by bytes downloaded.
        const auto peers = s->top_peers(10);
        if (!peers.empty())
        {
            os << "    ---- top peers (download) ----\n";
            for (const auto& p : peers)
            {
                os << "      " << std::setw(26) << std::left << p.address
                   << "  " << utils::format_byte_size(p.bytes) << "\n";
            }
        }

        // Print the peers we are currently seeding to (those we have served
        // data to), ordered by bytes uploaded.
        const auto up_peers = s->top_upload_peers(10);
        if (!up_peers.empty())
        {
            os << "    ---- seeding to (" << up_peers.size() << ") ----\n";
            for (const auto& p : up_peers)
            {
                os << "      " << std::setw(26) << std::left << p.address
                   << "  " << utils::format_byte_size(p.bytes) << "\n";
            }
        }
    }
    os << std::flush;
}

void SessionManager::print_status(std::ostream& os) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    print_status_locked(os);
}

void SessionManager::start_status_refresh(std::ostream& os,
                                          std::chrono::milliseconds interval)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_status_thread.joinable())
    {
        return;
    }
    m_status_stream = &os;
    m_status_interval = interval;
    m_status_stop.store(false, std::memory_order_release);
    m_status_thread = std::thread(&SessionManager::status_thread_main, this);
}

void SessionManager::stop_status_refresh()
{
    m_status_stop.store(true, std::memory_order_release);
    if (m_status_thread.joinable())
    {
        m_status_thread.join();
    }
    m_status_stream = nullptr;
}

bool SessionManager::status_refresh_running() const
{
    return m_status_thread.joinable();
}

void SessionManager::status_thread_main()
{
    while (!m_status_stop.load(std::memory_order_acquire))
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_status_stream)
            {
                *m_status_stream << k_clear_screen;
                print_status_locked(*m_status_stream);
            }
        }

        // Sleep in short slices so stop_status_refresh() is responsive.
        auto remaining = m_status_interval;
        constexpr auto k_slice = std::chrono::milliseconds(200);
        while (remaining > std::chrono::milliseconds::zero() &&
               !m_status_stop.load(std::memory_order_acquire))
        {
            const auto step = std::min(remaining, k_slice);
            std::this_thread::sleep_for(step);
            remaining -= step;
        }
    }
}

void SessionManager::set_metrics_output_dir(const std::string& dir)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_metrics_dir = dir;
    if (!m_metrics_dir.empty())
    {
        fs::create_directories(m_metrics_dir);
    }
}

std::string SessionManager::format_iso8601_utc(
    std::chrono::system_clock::time_point tp)
{
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

void SessionManager::append_metrics_csv_locked(size_t session_index) const
{
    if (m_metrics_dir.empty() || session_index >= m_sessions.size() ||
        !m_sessions[session_index])
    {
        return;
    }

    if (m_metrics_csv_header_written.size() <= session_index)
    {
        m_metrics_csv_header_written.resize(session_index + 1, false);
    }

    const TorrentManager& s = *m_sessions[session_index];
    const std::string base = utils::sanitize_filename(s.torrent_name());
    const fs::path csv_path =
        fs::path(m_metrics_dir) / (base + "_progress.csv");

    const bool write_header = !m_metrics_csv_header_written[session_index];

    std::ofstream out(csv_path, std::ios::app);
    if (!out)
    {
        return;
    }

    if (write_header)
    {
        out << "timestamp_iso,torrent_name,info_hash_hex,progress_pct,"
               "downloaded_bytes,total_bytes,speed_bps,up_speed_bps,"
               "dht_peers,workers_started,handshakes_ok,handshakes_failed,"
               "peer_run_failed,piece_hash_failures,elapsed_sec,state\n";
        m_metrics_csv_header_written[session_index] = true;
    }

    const auto now_system = std::chrono::system_clock::now();
    double elapsed_sec = 0.0;
    if (m_session_start_steady != std::chrono::steady_clock::time_point{})
    {
        elapsed_sec = std::chrono::duration<double>(
                          std::chrono::steady_clock::now() -
                          m_session_start_steady)
                          .count();
    }

    const double speed =
        (session_index < m_speeds_bps.size()) ? m_speeds_bps[session_index]
                                              : 0.0;
    const double up_speed = (session_index < m_up_speeds_bps.size())
                                ? m_up_speeds_bps[session_index]
                                : 0.0;

    std::string state = "downloading";
    if (s.is_complete())
    {
        state = s.is_seeding() ? "seeding" : "complete";
    }

    out << format_iso8601_utc(now_system) << ','
        << '"' << json_escape(s.torrent_name()) << '"' << ','
        << s.info_hash_hex() << ','
        << std::fixed << std::setprecision(4) << s.progress() << ','
        << s.downloaded_bytes() << ','
        << s.total_bytes() << ','
        << std::setprecision(2) << speed << ','
        << up_speed << ','
        << s.dht_peers_discovered() << ','
        << s.peer_workers_started() << ','
        << s.peer_handshakes_ok() << ','
        << s.peer_handshake_failed() << ','
        << s.peer_run_failed() << ','
        << s.piece_hash_failures() << ','
        << std::setprecision(3) << elapsed_sec << ','
        << state << '\n';
}

std::vector<SessionRunSummary> SessionManager::collect_run_summaries() const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    const auto end_steady = std::chrono::steady_clock::now();
    const auto end_system = std::chrono::system_clock::now();

    double duration_sec = 0.0;
    if (m_session_start_steady != std::chrono::steady_clock::time_point{})
    {
        duration_sec =
            std::chrono::duration<double>(end_steady - m_session_start_steady)
                .count();
    }

    const std::string started_iso =
        (m_session_start_system != std::chrono::system_clock::time_point{})
            ? format_iso8601_utc(m_session_start_system)
            : std::string{};
    const std::string completed_iso = format_iso8601_utc(end_system);

    std::vector<SessionRunSummary> out;
    out.reserve(m_sessions.size());

    for (size_t i = 0; i < m_sessions.size(); ++i)
    {
        const auto& s = m_sessions[i];
        if (!s)
        {
            continue;
        }

        SessionRunSummary summary;
        summary.torrent_name = s->torrent_name();
        summary.info_hash_hex = s->info_hash_hex();
        summary.started_at_iso = started_iso;
        summary.completed_at_iso = completed_iso;
        summary.duration_sec = duration_sec;
        summary.total_bytes = s->total_bytes();
        summary.downloaded_bytes = s->downloaded_bytes();
        summary.progress_pct = s->progress();
        summary.dht_peers_discovered = s->dht_peers_discovered();
        summary.peer_workers_started = s->peer_workers_started();
        summary.peer_handshakes_ok = s->peer_handshakes_ok();
        summary.peer_handshake_failed = s->peer_handshake_failed();
        summary.peer_run_failed = s->peer_run_failed();
        summary.piece_hash_failures = s->piece_hash_failures();
        summary.uploaded_bytes = s->uploaded_bytes();
        summary.blocks_uploaded = s->blocks_uploaded();
        summary.complete = s->is_complete();

        if (duration_sec > 0.0)
        {
            summary.avg_speed_bps =
                static_cast<double>(summary.downloaded_bytes) / duration_sec;
        }
        summary.peak_speed_bps =
            (i < m_peak_speeds_bps.size()) ? m_peak_speeds_bps[i] : 0.0;

        if (!m_metrics_dir.empty())
        {
            const std::string base =
                utils::sanitize_filename(summary.torrent_name);
            summary.progress_csv_path =
                (fs::path(m_metrics_dir) / (base + "_progress.csv")).string();
        }

        out.push_back(std::move(summary));
    }

    return out;
}

void SessionManager::write_run_summary_json(
    const SessionRunSummary& summary,
    const std::string& output_path,
    std::optional<bool> reference_match)
{
    fs::create_directories(fs::path(output_path).parent_path());

    std::ofstream out(output_path);
    if (!out)
    {
        return;
    }

    out << std::fixed << std::setprecision(4);
    out << "{\n";
    out << "  \"torrent_name\": \"" << json_escape(summary.torrent_name)
        << "\",\n";
    out << "  \"info_hash_hex\": \"" << json_escape(summary.info_hash_hex)
        << "\",\n";
    out << "  \"started_at\": \"" << json_escape(summary.started_at_iso)
        << "\",\n";
    out << "  \"completed_at\": \"" << json_escape(summary.completed_at_iso)
        << "\",\n";
    out << "  \"duration_sec\": " << std::setprecision(3) << summary.duration_sec
        << ",\n";
    out << std::setprecision(4);
    out << "  \"total_bytes\": " << summary.total_bytes << ",\n";
    out << "  \"downloaded_bytes\": " << summary.downloaded_bytes << ",\n";
    out << "  \"progress_pct\": " << summary.progress_pct << ",\n";
    out << "  \"avg_speed_bps\": " << std::setprecision(2) << summary.avg_speed_bps
        << ",\n";
    out << "  \"peak_speed_bps\": " << summary.peak_speed_bps << ",\n";
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
    out << "  \"uploaded_bytes\": " << summary.uploaded_bytes << ",\n";
    out << "  \"blocks_uploaded\": " << summary.blocks_uploaded << ",\n";
    out << "  \"complete\": " << (summary.complete ? "true" : "false") << ",\n";
    out << "  \"progress_csv\": \"" << json_escape(summary.progress_csv_path)
        << "\"";
    if (reference_match.has_value())
    {
        out << ",\n  \"reference_match\": "
            << (reference_match.value() ? "true" : "false");
    }
    out << "\n}\n";
}
