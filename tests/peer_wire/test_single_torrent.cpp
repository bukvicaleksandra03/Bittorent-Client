// Integration test: downloads a real torrent (long-running, uses network and
// disk).
//
// Not run by default (skipped) so `make test` stays fast. Enable with:
//   RUN_SINGLE_TORRENT_TEST=1 ./out/test_single_torrent
//
// Optional environment:
//   TORRENT_PATH           Path to .torrent (default: project path below)
//   TORRENT_TEST_OUT       Download directory (default:
//   .../torrent_files/downloaded) TORRENT_REFERENCE_DIR  Reference files from
//   qBittorrent (default:
//                          .../torrent_files/downloaded_using_qbittorent)
//   TORRENT_TEST_LOG       Log file path (default: single_torrent_test.log in
//   cwd)
//   TORRENT_LOG_CONSOLE    If set to 1, mirror structured logs to the terminal
//                          as well as the file (default: file only after
//                          set_file).
//   SINGLE_TORRENT_MAX_SEC Stop after N seconds (incomplete ok); 0 = until
//   done

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "bencode/bencode_parser.h"
#include "logger.h"
#include "peer_wire/session_manager.h"
#include "peer_wire/torrent_manager.h"

namespace fs = std::filesystem;

namespace
{

// std::string default_torrent_path()
// {
//     return "/home/albukvic/Projects/Bittorent-Client/torrent_files/"
//            "unparsed_torrents/"
//            "linuxmint-22.2-cinnamon-64bit.iso.torrent";
// }

std::string default_torrent_path()
{
    return "/home/albukvic/Projects/Bittorent-Client/torrent_files/"
           "unparsed_torrents/ubuntu-25.10-desktop-amd64.iso.torrent";
}

std::string default_reference_dir()
{
    return "/home/albukvic/Projects/Bittorent-Client/torrent_files/"
           "downloaded_using_qbittorent";
}

// Byte-for-byte compare (streaming, large-file safe).
bool files_equal(const fs::path& a, const fs::path& b)
{
    std::error_code ec;
    if (!fs::is_regular_file(a, ec) || !fs::is_regular_file(b, ec))
    {
        return false;
    }
    const auto sa = fs::file_size(a, ec);
    const auto sb = fs::file_size(b, ec);
    if (ec || sa != sb)
    {
        LOG_I("File size does not match the expected.");
        return false;
    }

    std::ifstream fa(a, std::ios::binary);
    std::ifstream fb(b, std::ios::binary);
    if (!fa || !fb)
    {
        return false;
    }

    constexpr size_t k_buf = 1024U * 1024U;
    std::vector<char> bufa(k_buf);
    std::vector<char> bufb(k_buf);

    while (fa && fb)
    {
        fa.read(bufa.data(), static_cast<std::streamsize>(bufa.size()));
        fb.read(bufb.data(), static_cast<std::streamsize>(bufb.size()));
        const std::streamsize ga = fa.gcount();
        const std::streamsize gb = fb.gcount();
        if (ga != gb)
        {
            return false;
        }
        if (std::memcmp(bufa.data(), bufb.data(), static_cast<size_t>(ga)) != 0)
        {
            return false;
        }
        if (ga == 0)
        {
            break;
        }
    }
    return true;
}

// Compare downloaded output to reference under reference_dir using the same
// name as the torrent display name (torrent_name):
// Single-file: got = output_dir/torrent_name, ref = reference_dir/torrent_name.
// Multi-file: got = output_dir/rel_path, ref =
// reference_dir/torrent_name/rel_path (qBittorrent-style tree under a folder
// named like the torrent).
void expect_download_matches_reference(
    const std::string& output_dir,
    const std::string& reference_dir,
    const std::string& torrent_name,
    bool is_multifile,
    const std::vector<std::pair<fs::path, uint64_t>>& file_layout)
{
    ASSERT_FALSE(torrent_name.empty())
        << "Torrent name empty; cannot locate reference.";

    if (!is_multifile)
    {
        const fs::path got = fs::path(output_dir) / torrent_name;
        const fs::path ref = fs::path(reference_dir) / torrent_name;

        ASSERT_TRUE(fs::exists(got)) << "Missing downloaded file: " << got;
        ASSERT_TRUE(fs::is_regular_file(ref))
            << "Missing reference file (place qBittorrent output in "
            << reference_dir << " as " << ref.filename().string() << ")";

        EXPECT_TRUE(files_equal(got, ref))
            << "Content mismatch: got " << got << " vs reference " << ref;
        return;
    }

    const fs::path ref_root = fs::path(reference_dir) / torrent_name;
    ASSERT_TRUE(fs::is_directory(ref_root))
        << "Missing reference directory (expected qBittorrent output under "
        << ref_root << ")";

    for (const auto& entry : file_layout)
    {
        const fs::path got = fs::path(output_dir) / entry.first;
        const fs::path ref = ref_root / entry.first;

        ASSERT_TRUE(fs::exists(got)) << "Missing downloaded file: " << got;
        ASSERT_TRUE(fs::exists(ref))
            << "Missing reference file under " << ref_root << ": " << ref;

        EXPECT_TRUE(files_equal(got, ref))
            << "Content mismatch: got " << got << " vs reference " << ref;
    }
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
    if (const char* c = std::getenv("TORRENT_LOG_CONSOLE");
        c != nullptr && std::strcmp(c, "1") == 0)
    {
        logger::Logger::instance().set_console_enabled(true);
    }

    LOG_INFO("Structured log file: " + log_file);
    std::cout << "Logging to file: " << log_file
              << " (set TORRENT_LOG_CONSOLE=1 to mirror logs to terminal)\n";

    BencodeParser parser(torrent_path);
    std::unique_ptr<TorrentFile> torrent = parser.parse();
    ASSERT_NE(torrent, nullptr) << "Failed to parse torrent";

    const std::string torrent_name = torrent->get_name();
    const bool is_multifile = torrent->is_multi_file();
    const std::vector<std::pair<fs::path, uint64_t>> file_layout =
        torrent->file_layout();

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

    SessionManager sessions;
    sessions.add(
        std::make_unique<TorrentManager>(std::move(torrent), output_dir, 6881));
    sessions.start_all();

    const auto status_refresh_interval = std::chrono::seconds(5);
    sessions.start_status_refresh(std::cout, status_refresh_interval);

    const auto start_time = std::chrono::steady_clock::now();
    bool timed_out = false;

    // Main thread only waits for completion / timeout; terminal updates run in
    // SessionManager's status thread.
    const auto wait_poll = std::chrono::seconds(1);
    while (!sessions.all_complete())
    {
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

        std::this_thread::sleep_for(wait_poll);
    }

    sessions.stop_all();

    if (timed_out)
    {
        LOG_INFO("Stopped early; data under: " + output_dir);
        GTEST_SKIP() << "Timed out before completion; see " << log_file;
    }

    EXPECT_TRUE(sessions.all_complete());

    const char* ref_env = std::getenv("TORRENT_REFERENCE_DIR");
    const std::string reference_dir =
        ref_env ? std::string(ref_env) : default_reference_dir();

    expect_download_matches_reference(
        output_dir, reference_dir, torrent_name, is_multifile, file_layout);

    LOG_INFO("Download finished and matched reference under " + reference_dir);
}
