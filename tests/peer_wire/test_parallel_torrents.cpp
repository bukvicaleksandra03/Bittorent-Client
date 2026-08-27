// Integration test: download multiple torrents concurrently in one process
// (shared SessionManager, DHT, listen port).
//
// Not run by `make test-fast` or `make test-integration`. Build with
// `make test_parallel_torrents`, then enable with:
//   RUN_PARALLEL_TORRENTS_TEST=1 ./out/test_parallel_torrents
// or use ./download_parallel_torrents.sh
//
// Optional environment:
//   TORRENT_PATHS          Colon-separated .torrent paths (overrides defaults)
//   TORRENT_DIR            Directory to scan (overrides default path list)
//   PARALLEL_TORRENT_COUNT Number of torrents to run (default: 3)
//   TORRENT_TEST_OUT       Base download directory
//   TORRENT_REFERENCE_DIR  Reference files from qBittorrent
//   TORRENT_LOG_DIR        Log directory
//   TORRENT_METRICS_DIR    CSV/JSON metrics
//   PARALLEL_TORRENT_MAX_SEC  Stop after N seconds (0 = until all complete)
//   TORRENT_SKIP_REFERENCE Skip reference compare (default: 1)

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
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

fs::path project_root()
{
    return fs::path(__FILE__).parent_path().parent_path().parent_path();
}

std::vector<std::string> default_torrent_paths()
{
    const fs::path dir =
        project_root() / "torrent_files" / "unparsed_torrents";
    return {
        (dir / "kali-linux-2026.2-installer-amd64.iso.torrent").string(),
        (dir / "debian-13.5.0-amd64-netinst.iso.torrent").string(),
        (dir / "debian-mac-13.5.0-amd64-netinst.iso.torrent").string(),
    };
}

std::vector<std::string> scan_torrent_dir(const fs::path& torrent_dir,
                                          size_t want)
{
    std::vector<fs::path> discovered;
    if (!fs::is_directory(torrent_dir))
    {
        return {};
    }

    for (const fs::directory_entry& entry :
         fs::directory_iterator(torrent_dir))
    {
        if (entry.is_regular_file() &&
            entry.path().extension() == ".torrent")
        {
            discovered.push_back(entry.path());
        }
    }

    std::sort(discovered.begin(), discovered.end());
    std::vector<std::string> paths;
    paths.reserve(std::min(want, discovered.size()));
    for (size_t i = 0; i < discovered.size() && i < want; ++i)
    {
        paths.push_back(discovered[i].string());
    }
    return paths;
}

std::string default_reference_dir()
{
    return (project_root() / "torrent_files" / "downloaded_using_qbittorent")
        .string();
}

struct TorrentJob
{
    std::string torrent_path;
    std::string output_dir;
    std::string torrent_name;
    bool is_multifile{false};
    std::vector<std::pair<fs::path, uint64_t>> file_layout;
};

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
            << "Missing reference file under " << reference_dir;

        EXPECT_TRUE(files_equal(got, ref))
            << "Content mismatch: got " << got << " vs reference " << ref;
        return;
    }

    const fs::path ref_root = fs::path(reference_dir) / torrent_name;
    ASSERT_TRUE(fs::is_directory(ref_root))
        << "Missing reference directory under " << ref_root;

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

bool skip_reference_check()
{
    const char* env = std::getenv("TORRENT_SKIP_REFERENCE");
    if (!env)
    {
        return true;
    }
    return env[0] != '\0' && env[0] != '0';
}

std::vector<std::string> split_colon_paths(const std::string& value)
{
    std::vector<std::string> paths;
    std::string current;
    for (char ch : value)
    {
        if (ch == ':')
        {
            if (!current.empty())
            {
                paths.push_back(current);
                current.clear();
            }
        }
        else
        {
            current.push_back(ch);
        }
    }
    if (!current.empty())
    {
        paths.push_back(current);
    }
    return paths;
}

size_t parallel_torrent_count()
{
    const char* env = std::getenv("PARALLEL_TORRENT_COUNT");
    if (!env || env[0] == '\0')
    {
        return 3;
    }
    const int parsed = std::atoi(env);
    return parsed > 0 ? static_cast<size_t>(parsed) : 3;
}

std::vector<std::string> resolve_torrent_paths()
{
    const size_t want = parallel_torrent_count();

    if (const char* paths_env = std::getenv("TORRENT_PATHS"))
    {
        return split_colon_paths(paths_env);
    }

    if (const char* dir_env = std::getenv("TORRENT_DIR"))
    {
        return scan_torrent_dir(fs::path(dir_env), want);
    }

    const std::vector<std::string> defaults = default_torrent_paths();
    std::vector<std::string> paths;
    paths.reserve(std::min(want, defaults.size()));
    for (size_t i = 0; i < defaults.size() && i < want; ++i)
    {
        paths.push_back(defaults[i]);
    }
    return paths;
}

std::string output_subdir(const std::string& base_dir,
                          const std::string& torrent_name)
{
    return (fs::path(base_dir) / utils::sanitize_filename(torrent_name))
        .string();
}

}  // namespace

TEST(ParallelTorrents, DownloadThreeConcurrently)
{
    if (!std::getenv("RUN_PARALLEL_TORRENTS_TEST"))
    {
        GTEST_SKIP()
            << "Set RUN_PARALLEL_TORRENTS_TEST=1 to run this integration test "
               "(long download).";
    }

    const size_t want_count = parallel_torrent_count();
    const std::vector<std::string> torrent_paths = resolve_torrent_paths();

    ASSERT_FALSE(torrent_paths.empty())
        << "No torrent paths resolved. Set TORRENT_PATHS, TORRENT_DIR, or "
           "ensure default torrents exist under torrent_files/unparsed_torrents.";
    ASSERT_GE(torrent_paths.size(), want_count)
        << "Need at least " << want_count
        << " torrent file(s). Found " << torrent_paths.size()
        << ". Set TORRENT_PATHS, lower PARALLEL_TORRENT_COUNT, or use "
           "TORRENT_DIR with more .torrent files.";

    const char* out_env = std::getenv("TORRENT_TEST_OUT");
    const std::string output_base =
        out_env ? std::string(out_env)
                : (project_root() / "torrent_files" / "downloaded" /
                   "parallel")
                      .string();
    fs::create_directories(output_base);

    const char* log_env = std::getenv("TORRENT_LOG_DIR");
    const std::string log_dir =
        log_env ? std::string(log_env) : (project_root() / "logs").string();
    fs::create_directories(log_dir);

    const char* metrics_env = std::getenv("TORRENT_METRICS_DIR");
    const std::string metrics_dir =
        metrics_env ? std::string(metrics_env)
                    : (project_root() / "logs" / "metrics").string();
    fs::create_directories(metrics_dir);

    int max_sec = 0;
    if (const char* m = std::getenv("PARALLEL_TORRENT_MAX_SEC"))
    {
        max_sec = std::atoi(m);
    }

    SessionManager sessions;
    sessions.set_metrics_output_dir(metrics_dir);

    std::vector<TorrentJob> jobs;
    jobs.reserve(want_count);

    for (size_t i = 0; i < want_count; ++i)
    {
        const std::string& torrent_path = torrent_paths[i];
        ASSERT_TRUE(fs::exists(torrent_path))
            << "Torrent file not found: " << torrent_path;

        bencode::Parser parser(torrent_path);
        std::unique_ptr<TorrentFile> torrent = parser.parse();
        ASSERT_NE(torrent, nullptr)
            << "Failed to parse torrent: " << torrent_path;

        TorrentJob job;
        job.torrent_path = torrent_path;
        job.torrent_name = torrent->get_name();
        job.is_multifile = torrent->is_multi_file();
        job.file_layout = torrent->file_layout();
        job.output_dir = output_subdir(output_base, job.torrent_name);
        fs::create_directories(job.output_dir);

        std::cout << "Torrent [" << (i + 1) << "/" << want_count << "]: "
                  << torrent_path << "\n";
        std::cout << "  name: " << job.torrent_name << "\n";
        std::cout << "  output: " << job.output_dir << "\n";
        std::cout << "  info hash: " << torrent->get_info_hash_hex() << "\n";

        sessions.add(std::make_unique<TorrentManager>(
            std::move(torrent), job.output_dir, log_dir,
            logger::Level::DEBUG));
        jobs.push_back(std::move(job));
    }

    EXPECT_EQ(sessions.session_count(), want_count);

    std::cout << "Starting " << want_count
              << " torrent(s) concurrently in one SessionManager...\n";

    sessions.start_all();
    sessions.start_status_refresh(std::cout, std::chrono::seconds(5));

    const auto start_time = std::chrono::steady_clock::now();
    bool timed_out = false;

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
                std::cout << "PARALLEL_TORRENT_MAX_SEC reached; stopping "
                             "workers (downloads may be incomplete)\n";
                break;
            }
        }

        std::this_thread::sleep_for(wait_poll);
    }

    if (timed_out)
    {
        sessions.stop_all();
        std::cout << "Stopped early; partial data under: " << output_base
                  << "\n";
        GTEST_SKIP() << "Timed out before all torrents completed.";
    }

    EXPECT_TRUE(sessions.all_complete());

    if (!skip_reference_check())
    {
        const char* ref_env = std::getenv("TORRENT_REFERENCE_DIR");
        const std::string reference_dir =
            ref_env ? std::string(ref_env) : default_reference_dir();

        for (const TorrentJob& job : jobs)
        {
            expect_download_matches_reference(
                job.output_dir, reference_dir, job.torrent_name,
                job.is_multifile, job.file_layout);
        }

        sessions.set_reference_match(true);
        std::cout << "All downloads finished and matched reference under "
                  << reference_dir << "\n";
    }
    else
    {
        std::cout << "All downloads finished (reference check skipped).\n";
    }

    sessions.stop_all();
}
