#include <iostream>
#include <memory>
#include <sstream>
#include <string>

#include "bencode_parser.h"
#include "logger.h"
#include "socket.h"
#include "torrent_file.h"
#include "tracker_request.h"

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;  // Suppress unused warnings

    logger::Logger::instance().set_level(logger::Level::DEBUG);

    try
    {
        std::string torrent_to_download =
            "./torrents/ubuntu-25.10-desktop-amd64.iso.torrent";

        BencodeParser parser(torrent_to_download);
        std::unique_ptr<TorrentFile> tf = parser.parse();

        std::ostringstream oss;
        tf->print(oss);
        LOG_I("Torrent info:\n" + oss.str());

        TrackerRequest tracker_request(tf);

        // dns_lookup(host, port);
        tracker_request.send();
    }
    catch (const std::exception& e)
    {
        LOG_E("Error: " + std::string(e.what()));
        return 1;
    }
    catch (...)
    {
        LOG_E("Unknown error occurred");
        return 1;
    }

    return 0;
}