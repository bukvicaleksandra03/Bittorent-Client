// Integration test: downloads a real torrent (long-running, uses network and
// disk).
//
// Not run by `make test-fast` or `make test-integration`. Build with
// `make test_single_torrent`, then enable with:
//   RUN_SINGLE_TORRENT_TEST=1 ./out/test_single_torrent
// or use ./download_torrent.sh
//
// Optional environment:
//   TORRENT_PATH           Path to .torrent
//   TORRENT_TEST_OUT Download
//   directory (default: <project>/torrent_files/downloaded)
//   TORRENT_REFERENCE_DIR  Reference files from qBittorrent
//                          (default:
//                          <project>/torrent_files/downloaded_using_qbittorent)
//   TORRENT_LOG_DIR        Log directory (default: <project>/logs)
//   TORRENT_METRICS_DIR    CSV/JSON metrics (default: <project>/logs/metrics)
//   SINGLE_TORRENT_MAX_SEC Stop after N seconds (incomplete ok); 0 = until done

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "bencode/bencode_parser.h"
#include "logger.h"
#include "peer_wire/session_manager.h"
#include "peer_wire/torrent_manager.h"
#include "utils.h"

namespace fs = std::filesystem;

namespace
{

// Resolve the project root at compile time from the location of this source
// file: tests/peer_wire/test_single_torrent.cpp  ->  ../../  = project root.
fs::path project_root()
{
    // __FILE__ is the absolute path to this source file as seen by the
    // compiler.
    return fs::path(__FILE__).parent_path().parent_path().parent_path();
}

std::string default_torrent_path()
{
    return (project_root() / "torrent_files" / "unparsed_torrents" /
            "linuxmint-22.2-cinnamon-64bit.iso.torrent")
        .string();
}

std::string default_reference_dir()
{
    return (project_root() / "torrent_files" / "downloaded_using_qbittorent")
        .string();
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
        std::cout << "File size does not match the expected.\n";
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

void write_run_summaries(
    const SessionManager& sessions,
    const std::string& metrics_dir,
    std::optional<bool> reference_match = std::nullopt)
{
    if (metrics_dir.empty())
    {
        return;
    }

    for (const SessionRunSummary& summary : sessions.collect_run_summaries())
    {
        const std::string base = utils::sanitize_filename(summary.torrent_name);
        const fs::path json_path =
            fs::path(metrics_dir) / (base + "_summary.json");
        SessionManager::write_run_summary_json(
            summary, json_path.string(), reference_match);
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

    // Per-torrent and per-peer log files are created automatically by
    // TorrentManager under <log_dir>/<name>.log (and peer variants).
    // The log level can be tuned here by changing the Level argument below.

    bencode::Parser parser(torrent_path);
    std::unique_ptr<TorrentFile> torrent = parser.parse();
    ASSERT_NE(torrent, nullptr) << "Failed to parse torrent";

    const std::string torrent_name = torrent->get_name();
    const bool is_multifile = torrent->is_multi_file();
    const std::vector<std::pair<fs::path, uint64_t>> file_layout =
        torrent->file_layout();

    const char* out_env = std::getenv("TORRENT_TEST_OUT");
    const std::string output_dir =
        out_env ? std::string(out_env)
                : (project_root() / "torrent_files" / "downloaded").string();
    fs::create_directories(output_dir);

    const char* log_env = std::getenv("TORRENT_LOG_DIR");
    const std::string log_dir =
        log_env ? std::string(log_env) : (project_root() / "logs").string();
    fs::create_directories(log_dir);

    const char* metrics_env = std::getenv("TORRENT_METRICS_DIR");
    const std::string metrics_dir =
        metrics_env ? std::string(metrics_env)
                    : (project_root() / "logs" / "metrics").string();
    fs::create_directories(metrics_dir);

    std::cout << "Output directory: " << output_dir << "\n";
    std::cout << "Log directory:    " << log_dir << "\n";
    std::cout << "Metrics directory: " << metrics_dir << "\n";
    std::cout << "Info hash: " << torrent->get_info_hash_hex() << "\n";

    int max_sec = 0;
    if (const char* m = std::getenv("SINGLE_TORRENT_MAX_SEC"))
    {
        max_sec = std::atoi(m);
    }

    SessionManager sessions;
    sessions.set_metrics_output_dir(metrics_dir);
    sessions.add(std::make_unique<TorrentManager>(
        std::move(torrent), output_dir, log_dir, logger::Level::DEBUG));
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
                std::cout << "SINGLE_TORRENT_MAX_SEC reached; stopping workers "
                             "(download may be incomplete)\n";
                break;
            }
        }

        std::this_thread::sleep_for(wait_poll);
    }

    sessions.stop_all();
    write_run_summaries(sessions, metrics_dir);

    if (timed_out)
    {
        std::cout << "Stopped early; data under: " << output_dir << "\n";
        GTEST_SKIP() << "Timed out before completion.";
    }

    EXPECT_TRUE(sessions.all_complete());

    const char* ref_env = std::getenv("TORRENT_REFERENCE_DIR");
    const std::string reference_dir =
        ref_env ? std::string(ref_env) : default_reference_dir();

    expect_download_matches_reference(
        output_dir, reference_dir, torrent_name, is_multifile, file_layout);

    write_run_summaries(sessions, metrics_dir, true);

    std::cout << "Download finished and matched reference under "
              << reference_dir << "\n";
}
