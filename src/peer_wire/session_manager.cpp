#include "peer_wire/session_manager.h"

#include <algorithm>
#include <iomanip>
#include <thread>

#include "peer_wire/torrent_manager.h"
#include "utils.h"

namespace
{
// Clear screen and move cursor home (ANSI). Works in most modern terminals.
constexpr const char k_clear_screen[] = "\033[2J\033[H";

}  // namespace

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
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& s : m_sessions)
    {
        if (s)
        {
            s->start();
        }
    }
}

void SessionManager::stop_all()
{
    stop_status_refresh();
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
//     ubuntu-22.04-desktop-amd64.iso           0.25%  (13.00 MiB / 5.20 GiB)  5.20 MiB/s  active
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

    if (m_last_snapshot == std::chrono::steady_clock::time_point{})
    {
        // First call: seed prev_bytes so the next interval gives a clean delta.
        for (size_t i = 0; i < m_sessions.size(); ++i)
        {
            if (m_sessions[i])
            {
                m_prev_bytes[i] = m_sessions[i]->downloaded_bytes();
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

        os << "  " << std::setw(48) << std::left
           << s->torrent_name().substr(0, 48) << std::fixed << std::setprecision(2)
           << std::setw(7) << std::right << s->progress() << "%  "
           << std::setw(24) << std::left << size_info
           << "  " << std::setw(14) << std::left << speed_str
           << "  " << (s->is_complete() ? "done" : "active") << "\n";

        // Print top 10 peers by bytes downloaded.
        const auto peers = s->top_peers(10);
        if (!peers.empty())
        {
            os << "    ---- top peers ----\n";
            for (const auto& p : peers)
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
