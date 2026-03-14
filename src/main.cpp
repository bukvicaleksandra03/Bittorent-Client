#include <iostream>
#include <memory>
#include <sstream>
#include <string>

#include "bencode_parser.h"
#include "logger.h"
#include "socket.h"
#include "torrent_file.h"
#include "tracker_factory.h"
#include "tracker_request.h"
#include "tracker_response.h"
#include "utils.h"

int main(int argc, char* argv[])
{
    logger::Logger::instance().set_level(logger::Level::DEBUG);

    std::string torrent_to_download;

    if (argc >= 2)
    {
        torrent_to_download = argv[1];
    }
    else
    {
        torrent_to_download =
            "./torrents/ubuntu-25.10-desktop-amd64.iso.torrent";
    }

    try
    {
        LOG_I("Loading torrent: " + torrent_to_download);

        BencodeParser parser(torrent_to_download);
        std::unique_ptr<TorrentFile> tf = parser.parse();

        std::ostringstream oss;
        tf->print(oss);
        LOG_I("Torrent info:\n" + oss.str());

        // Generate our peer ID
        PeerId peer_id = utils::generate_peer_id();
        LOG_I("Peer ID: " + utils::to_hex(peer_id));

        // Contact tracker
        std::unique_ptr<TrackerRequest> tracker_req;

        tracker_req = create_tracker_request(peer_id, tf);

        TrackerResponse response = tracker_req->send();
        LOG_I("Tracker response:\n" + response.to_string());
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
