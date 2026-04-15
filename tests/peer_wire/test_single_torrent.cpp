// Integration test: downloads a real torrent (long-running, uses network and
// disk).
//
// Not run by default (skipped) so `make test` stays fast. Enable with:
//   RUN_SINGLE_TORRENT_TEST=1 ./test_single_torrent
//
// Optional environment:
//   TORRENT_PATH          Path to .torrent (default: project path below)
//   TORRENT_TEST_OUT      Download directory (default:
//   /tmp/bittorrent-single-test) TORRENT_TEST_LOG      Log file path (default:
//   single_torrent_test.log in cwd) SINGLE_TORRENT_MAX_SEC  Stop after N
//   seconds (incomplete ok); 0 = until done

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "bencode/bencode_parser.h"
#include "logger.h"
#include "peer_wire/torrent_manager.h"

namespace fs = std::filesystem;

namespace
{

std::string default_torrent_path()
{
    return "/home/albukvic/Projects/Bittorent-Client/torrent_files/"
           "unparsed_torrents/"
           "linuxmint-22.2-cinnamon-64bit.iso.torrent";
}

}  // namespace

TEST(SingleTorrent, DownloadWithProgressLogging)
{
    if (!std::getenv("RUN_SINGLE_TORRENT_TEST"))
    {
        GTEST_SKIP()
            << "Set RUN_SINGLE_TORRENT_TEST=1 to run this integration test "
               "(long download).";
    }

    const char* path_env = std::getenv("TORRENT_PATH");
    const std::string torrent_path =
        path_env ? std::string(path_env) : default_torrent_path();

    ASSERT_TRUE(fs::exists(torrent_path))
        << "Torrent file not found: " << torrent_path;

    logger::Logger::instance().set_level(logger::Level::DEBUG);

    const char* log_env = std::getenv("TORRENT_TEST_LOG");
    const std::string log_file =
        log_env ? std::string(log_env) : "single_torrent_test.log";
    logger::Logger::instance().set_file(log_file);

    LOG_INFO("Log file (duplicate of console, no ANSI): " + log_file);
    std::cout << "Logging to file: " << log_file << std::endl;

    BencodeParser parser(torrent_path);
    std::unique_ptr<TorrentFile> torrent = parser.parse();
    ASSERT_NE(torrent, nullptr) << "Failed to parse torrent";

    const char* out_env = std::getenv("TORRENT_TEST_OUT");
    const std::string output_dir =
        out_env ? std::string(out_env)
                : "/home/albukvic/Projects/Bittorent-Client/torrent_files/"
                  "downloaded";
    fs::create_directories(output_dir);

    LOG_INFO("Output directory: " + output_dir);
    LOG_INFO("Info hash: " + torrent->get_info_hash_hex());

    int max_sec = 0;
    if (const char* m = std::getenv("SINGLE_TORRENT_MAX_SEC"))
    {
        max_sec = std::atoi(m);
    }

    TorrentManager manager(std::move(torrent), output_dir, 6881);
    manager.start();

    const auto poll_interval = std::chrono::seconds(5);
    const auto start_time = std::chrono::steady_clock::now();
    bool timed_out = false;

    while (!manager.is_complete())
    {
        const double pct = manager.progress();
        std::string msg = "Progress: " + std::to_string(pct) + "% complete";
        LOG_INFO(msg);
        std::cout << msg << std::endl;

        if (max_sec > 0)
        {
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - start_time)
                    .count();
            if (elapsed >= max_sec)
            {
                timed_out = true;
                LOG_W(
                    "SINGLE_TORRENT_MAX_SEC reached; stopping workers "
                    "(download may be incomplete)");
                break;
            }
        }

        std::this_thread::sleep_for(poll_interval);
    }

    manager.stop();

    if (timed_out)
    {
        LOG_INFO("Stopped early; data under: " + output_dir);
        GTEST_SKIP() << "Timed out before completion; see " << log_file;
    }

    EXPECT_TRUE(manager.is_complete());
    LOG_INFO("Download finished.");
}
