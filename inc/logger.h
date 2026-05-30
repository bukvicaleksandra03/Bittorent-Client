#pragma once

#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>

namespace logger
{

enum class Level
{
    DEBUG = 0,
    INFO = 1,
    WARNING = 2,
    ERROR = 3,
    NONE = 4  // Disables all logging
};

class Logger
{
   public:
    Logger()
        : min_level(Level::INFO), log_to_file(false), m_console_enabled(true)
    {
    }

    ~Logger()
    {
        if (file_stream.is_open())
            file_stream.close();
    }

    void set_level(Level level)
    {
        min_level = level;
    }
    Level get_level() const
    {
        return min_level;
    }

    // Optional prefix prepended to every message (e.g. "[ubuntu.iso] " or
    // "[1.2.3.4:6881] "). Useful to tell per-torrent / per-peer logs apart
    // when they share a console or are merged for review.
    void set_prefix(const std::string& prefix)
    {
        m_prefix = prefix;
    }

    // When a log file is opened successfully, console output is disabled by
    // default (logs go to the file only). Call set_console_enabled(true) to
    // mirror logs to the terminal as well.
    void set_file(const std::string& path)
    {
        if (file_stream.is_open())
        {
            file_stream.close();
        }
        // Create parent directories so the open never silently fails.
        std::filesystem::path p(path);
        if (p.has_parent_path())
        {
            std::filesystem::create_directories(p.parent_path());
        }
        file_stream.open(path, std::ios::app);
        log_to_file = file_stream.is_open();
        if (log_to_file)
        {
            m_console_enabled = false;
        }
    }

    void set_console_enabled(bool enabled)
    {
        m_console_enabled = enabled;
    }

    bool console_enabled() const
    {
        return m_console_enabled;
    }

    void log(Level level,
             const std::string& message,
             const char* file = nullptr,
             int line = 0)
    {
        if (level < min_level)
            return;

        // Each Logger instance has its own mutex; peer threads writing to
        // separate Logger objects never contend with each other.
        std::lock_guard<std::mutex> lock(m_log_mutex);

        std::ostringstream oss;
        oss << "[" << timestamp() << "] " << "[" << level_string(level) << "] ";

        if (!m_prefix.empty())
        {
            oss << m_prefix;
        }

        if (file)
        {
            oss << "[" << file << ":" << line << "] ";
        }

        oss << message;

        std::string output = oss.str();

        if (m_console_enabled)
        {
            std::ostream& out =
                (level >= Level::WARNING) ? std::cerr : std::cout;
            out << color_code(level) << output << "\033[0m" << std::endl;
        }

        if (log_to_file && file_stream.is_open())
        {
            file_stream << output << std::endl;
        }
    }

    // ---------------------------------------------------------------------------
    // Convenience wrappers – avoid repeating the level enum at call sites.
    // ---------------------------------------------------------------------------

    void debug(const std::string& msg, const char* file = nullptr, int line = 0)
    {
        log(Level::DEBUG, msg, file, line);
    }
    void info(const std::string& msg, const char* file = nullptr, int line = 0)
    {
        log(Level::INFO, msg, file, line);
    }
    void warning(const std::string& msg,
                 const char* file = nullptr,
                 int line = 0)
    {
        log(Level::WARNING, msg, file, line);
    }
    void error(const std::string& msg, const char* file = nullptr, int line = 0)
    {
        log(Level::ERROR, msg, file, line);
    }

    // Logs at ERROR level and throws std::runtime_error with the same message.
    [[noreturn]] void throw_error(const std::string& msg,
                                  const char* file = nullptr,
                                  int line = 0)
    {
        log(Level::ERROR, msg, file, line);
        throw std::runtime_error(msg);
    }

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

   private:
    Level min_level;
    bool log_to_file;
    bool m_console_enabled;
    std::string m_prefix;
    std::ofstream file_stream;
    std::mutex m_log_mutex;

    static std::string timestamp()
    {
        auto now = std::time(nullptr);
        auto tm = *std::localtime(&now);
        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
        return oss.str();
    }

    static const char* level_string(Level level)
    {
        switch (level)
        {
            case Level::DEBUG:
                return "DEBUG";
            case Level::INFO:
                return "INFO ";
            case Level::WARNING:
                return "WARN ";
            case Level::ERROR:
                return "ERROR";
            default:
                return "?????";
        }
    }

    static const char* color_code(Level level)
    {
        switch (level)
        {
            case Level::DEBUG:
                return "\033[36m";  // Cyan
            case Level::INFO:
                return "\033[32m";  // Green
            case Level::WARNING:
                return "\033[33m";  // Yellow
            case Level::ERROR:
                return "\033[31m";  // Red
            default:
                return "\033[0m";  // Reset
        }
    }
};

// ---------------------------------------------------------------------------
// Instance-based macros – pass the dereferenced logger as the first argument:
//   LLOG_INFO(*m_logger, "message")
//   LLOG_DEBUG(*peer_logger, "received block " + ...)
// ---------------------------------------------------------------------------

#define LLOG_DEBUG(lg, msg) (lg).debug((msg), __FILE__, __LINE__)
#define LLOG_INFO(lg, msg) (lg).info((msg), __FILE__, __LINE__)
#define LLOG_WARNING(lg, msg) (lg).warning((msg), __FILE__, __LINE__)
#define LLOG_ERROR(lg, msg) (lg).error((msg), __FILE__, __LINE__)
#define LLOG_THROW(lg, msg) (lg).throw_error((msg), __FILE__, __LINE__)

}  // namespace logger
