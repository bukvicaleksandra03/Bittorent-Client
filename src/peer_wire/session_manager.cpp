#include "peer_wire/session_manager.h"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <thread>

#include <filesystem>

#include "dht/dht_client.h"
#include "peer_address.h"
#include "peer_wire/torrent_manager.h"
#include "utils.h"

namespace fs = std::filesystem;

namespace
{
// Clear screen and move cursor home (ANSI). Works in most modern terminals.
constexpr const char k_clear_screen[] = "\033[2J\033[H";

}  // namespace

SessionManager::SessionManager(uint16_t listen_port)
    : m_listen_port(listen_port)
{
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
    if (m_log_output_dir.empty())
    {
        m_log_output_dir = manager->log_output_dir();
    }
    m_sessions.push_back(std::move(manager));
}

void SessionManager::start_all()
{
    // Attempt a single UPnP port mapping for the shared listen port.
    // Best-effort: we continue even if it fails (outbound connections still
    // function without an open inbound mapping).
    if (!m_upnp_mapping)
    {
        m_upnp_mapping = UPnPPortMapping::create(m_listen_port);
        if (m_upnp_mapping)
        {
            std::cout << "[SessionManager] UPnP: mapped port "
                      << m_upnp_mapping->external_port() << " -> "
                      << m_listen_port
                      << " (external IP: " << m_upnp_mapping->external_ip()
                      << ")\n";
        }
        else
        {
            std::cout << "[SessionManager] UPnP: no gateway or mapping failed"
                         " – continuing without port mapping\n";
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
            [this](const std::string& info_hash_hex,
                   const std::string& ip,
                   uint16_t port)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                for (auto& s : m_sessions)
                {
                    if (s && s->info_hash_hex() == info_hash_hex)
                    {
                        PeerAddress p;
                        p.ip = ip;
                        p.port = port;
                        s->add_dht_peer(p);
                    }
                }
            });

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const std::string log_dir =
                m_log_output_dir.empty() ? "logs" : m_log_output_dir;
            fs::create_directories(log_dir);
            const fs::path dht_log_path = fs::path(log_dir) / "dht.log";
            if (fs::exists(dht_log_path))
            {
                fs::remove(dht_log_path);
            }

            logger::Level level = logger::Level::DEBUG;
            if (!m_sessions.empty() && m_sessions.front())
            {
                level = m_sessions.front()->log_level();
            }

            m_dht_logger = std::make_shared<logger::Logger>();
            m_dht_logger->set_level(level);
            m_dht_logger->set_prefix("KRPC ");
            m_dht_logger->set_file(dht_log_path.string());
            m_dht_client->set_dht_logger(m_dht_logger);
        }

        m_dht_client->start();
        std::cout << "[SessionManager] DHT: started on port " << m_listen_port
                  << " (node id: " << m_dht_client->self_id().hex() << ")\n";
    }

    // Register active torrents for periodic DHT get_peers + announce_peer.
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& s : m_sessions)
        {
            if (s)
            {
                m_dht_client->register_torrent(s->info_hash_str(),
                                               m_listen_port);
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
            }
            m_last_snapshot = now;
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
