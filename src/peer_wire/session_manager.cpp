#include "peer_wire/session_manager.h"

#include <algorithm>
#include <iomanip>
#include <thread>

#include "peer_wire/torrent_manager.h"

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

void SessionManager::print_status_locked(std::ostream& os) const
{
    os << "---- sessions (" << m_sessions.size() << ") ----\n";
    for (const auto& s : m_sessions)
    {
        if (!s)
        {
            continue;
        }
        os << "  " << std::setw(48) << std::left << s->torrent_name().substr(0, 48)
           << std::fixed << std::setprecision(2) << std::setw(8) << std::right
           << s->progress() << "%  "
           << (s->is_complete() ? "done" : "active") << "\n";
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
