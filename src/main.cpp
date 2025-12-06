#include <iostream>
#include <memory>
#include <sstream>
#include <string>

#include "bencode_parser.h"
#include "logger.h"
#include "torrent_file.h"

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;  // Suppress unused warnings

    logger::Logger::instance().set_level(logger::Level::DEBUG);

    std::string torrent_to_download =
        "./torrents/ubuntu-25.10-desktop-amd64.iso.torrent";

    BencodeParser parser(torrent_to_download);
    std::unique_ptr<TorrentFile> tf = parser.parse();

    // Print to log using stringstream
    std::ostringstream oss;
    tf->print(oss);
    LOG_I("Torrent info:\n" + oss.str());

    return 0;
}