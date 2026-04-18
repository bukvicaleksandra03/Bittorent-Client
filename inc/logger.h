#pragma once

#include <ctime>
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
    static Logger& instance()
    {
        static Logger logger;
        return logger;
    }

    void set_level(Level level)
    {
        min_level = level;
    }
    Level get_level() const
    {
        return min_level;
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

        // Serialize all logging: multiple peer/tracker threads call LOG_* concurrently;
        // without a lock, file_stream / cout writes interleave and lines merge.
        std::lock_guard<std::mutex> lock(m_log_mutex);

        std::ostringstream oss;
        oss << "[" << timestamp() << "] " << "[" << level_string(level) << "] ";

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

   private:
    Logger()
        : min_level(Level::INFO), log_to_file(false), m_console_enabled(true)
    {
    }
    ~Logger()
    {
        if (file_stream.is_open())
            file_stream.close();
    }

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    Level min_level;
    bool log_to_file;
    bool m_console_enabled;
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

// Convenience macros with file/line info
#define LOG_DEBUG(msg)              \
    logger::Logger::instance().log( \
        logger::Level::DEBUG, msg, __FILE__, __LINE__)
#define LOG_INFO(msg) \
    logger::Logger::instance().log(logger::Level::INFO, msg, __FILE__, __LINE__)
#define LOG_WARNING(msg)            \
    logger::Logger::instance().log( \
        logger::Level::WARNING, msg, __FILE__, __LINE__)
#define LOG_ERROR(msg)              \
    logger::Logger::instance().log( \
        logger::Level::ERROR, msg, __FILE__, __LINE__)

#define LOG_AND_THROW(msg)                                   \
    do                                                       \
    {                                                        \
        auto _msg = (msg);                                   \
        logger::Logger::instance().log(                      \
            logger::Level::ERROR, _msg, __FILE__, __LINE__); \
        throw std::runtime_error(_msg);                      \
    } while (0)

// Simple macros without file/line (cleaner output)
#define LOG_D(msg) logger::Logger::instance().log(logger::Level::DEBUG, msg)
#define LOG_I(msg) logger::Logger::instance().log(logger::Level::INFO, msg)
#define LOG_W(msg) logger::Logger::instance().log(logger::Level::WARNING, msg)
#define LOG_E(msg) logger::Logger::instance().log(logger::Level::ERROR, msg)

}  // namespace logger
