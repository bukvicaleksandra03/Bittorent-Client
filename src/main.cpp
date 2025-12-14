#include <iostream>
#include <memory>
#include <sstream>
#include <string>

#include "bencode_parser.h"
#include "logger.h"
#include "socket.h"
#include "torrent_file.h"
#include "tracker_request.h"
#include "utils.h"

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;  // Suppress unused warnings

    logger::Logger::instance().set_level(logger::Level::DEBUG);

    try
    {
        std::string torrent_to_download =
            "./torrents/forrest-gump-ost-1994_archive.torrent";

        BencodeParser parser(torrent_to_download);
        std::unique_ptr<TorrentFile> tf = parser.parse();

        std::ostringstream oss;
        tf->print(oss);
        LOG_I("Torrent info:\n" + oss.str());

        PeerId peer_id = utils::generate_peer_id();
        LOG_I("Peer ID: " + utils::to_hex(peer_id));

        TrackerRequest tracker_req(peer_id, tf);

        std::vector<std::unique_ptr<Address>> addresses =
            dns_lookup(tracker_req.get_tracker_hostname(),
                       std::to_string(tracker_req.get_tracker_port()));

        std::stringstream ss;
        for (const auto& address : addresses)
        {
            ss << address->identifier << std::endl;
        }
        LOG_I("Tracker addresses:\n" + ss.str());

        tracker_req.send(addresses);
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