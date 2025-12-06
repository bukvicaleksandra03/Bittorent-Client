#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
namespace fs = std::filesystem;

#include "bencode_parser.h"
#include "bencode_types.h"
#include "logger.h"
#include "torrent_file.h"

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;  // Suppress unused warnings

    // Optional: Enable debug logging
    logger::Logger::instance().set_level(logger::Level::DEBUG);

    LOG_I("Parser test starting...");

    try
    {
        std::string torrents_dir = "./torrents";
        std::string parsed_bencode_dir = "./parsed_bencode";
        std::string parsed_torrents_dir = "./parsed_torrents";

        // Clear the output directories
        fs::remove_all(parsed_bencode_dir);
        fs::remove_all(parsed_torrents_dir);
        fs::create_directory(parsed_bencode_dir);
        fs::create_directory(parsed_torrents_dir);

        int count = 0;
        for (const auto& file : fs::directory_iterator(torrents_dir))
        {
            LOG_D("Parsing file: " + file.path().filename().string());

            BencodeParser parser(file.path());
            std::unique_ptr<TorrentFile> tf = parser.parse();

            fs::path parsed_bencode_file =
                fs::path(parsed_bencode_dir) / file.path().filename();
            std::ofstream out_file(parsed_bencode_file);
            parser.print(out_file);
            out_file.close();
            LOG_D("Saved parsed bencode output to: " +
                  parsed_bencode_file.string());

            fs::path parsed_torrent_file =
                fs::path(parsed_torrents_dir) / file.path().filename();
            out_file.open(parsed_torrent_file, std::ios::out);
            tf->print(out_file);
            out_file.close();
            LOG_D("Saved parsed torrent output to: " +
                  parsed_torrent_file.string());

            count++;
        }

        LOG_I("Successfully processed " + std::to_string(count) +
              " torrent(s)");
    }
    catch (const std::exception& e)
    {
        LOG_E(std::string("Fatal error: ") + e.what());
        return 1;
    }

    return 0;
}