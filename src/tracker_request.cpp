#include "tracker_request.h"

#include <iostream>
#include <stdexcept>

#include "http_request.h"
#include "logger.h"
#include "socket.h"

TrackerRequest::TrackerRequest(const PeerId& peer_id,
                               const std::unique_ptr<TorrentFile>& tf)
{
    extract_host_name_port_and_path(tf->get_announce());
    _info_hash = tf->get_info_hash();
    _peer_id = peer_id;
    _peer_ip = 0;
    _peer_port = 6881;  // Default BitTorrent port
    _downloaded = 0;
    _left = tf->get_total_size();
    _uploaded = 0;
    _event = 0;  // Started event
}

TrackerRequest::~TrackerRequest() = default;

std::string TrackerRequest::build_request() const
{
    return HttpRequest()
        .method(HttpRequest::Method::GET)
        .host(_tracker_hostname)
        .path(_tracker_path)
        .query("info_hash", std::string(_info_hash.begin(), _info_hash.end()))
        .query("peer_id", std::string(_peer_id.begin(), _peer_id.end()))
        .query("port", std::to_string(_peer_port))
        .query("uploaded", std::to_string(_uploaded))
        .query("downloaded", std::to_string(_downloaded))
        .query("left", std::to_string(_left))
        .query("compact", "1")
        .query("event", "started")
        .header("User-Agent", "BitTorrent-Client/1.0")
        .header("Accept", "*/*")
        .build();
}

void TrackerRequest::send(
    const std::vector<std::unique_ptr<Address>>& addresses)
{
    std::string request = build_request();

    LOG_D("Sending tracker request to " + _tracker_hostname + ":" +
          std::to_string(_tracker_port));
    LOG_D("Request:\n" + request);

    for (const auto& address : addresses)
    {
        Socket socket(address->domain());
        socket.connect(*address);
        socket.send(request.c_str(), request.size());

        std::string response;
        char buffer[4096];
        ssize_t bytes_read;
        while ((bytes_read = socket.read(buffer, sizeof(buffer) - 1)) > 0)
        {
            buffer[bytes_read] = '\0';
            response += buffer;
        }

        LOG_D("Response:\n" + response);
    }
}

void TrackerRequest::extract_host_name_port_and_path(
    const std::string& announce)
{
    // Extract hostname from announce URL
    // e.g., "https://torrent.ubuntu.com/announce" -> "torrent.ubuntu.com"
    // Also extract port if it is present
    // e.g., "https://torrent.ubuntu.com:8080/announce" -> "8080"
    // if no port is present, use default port 80 for http and 443 for https.
    // Also extract path if it is present
    // e.g., "https://torrent.ubuntu.com/announce" -> "/announce"
    // if no path is present, use default path "/"

    size_t start = announce.find("://");

    _tracker_port = 0;
    _tracker_path = "/";
    if (start != std::string::npos)
    {
        start += 3;  // Skip "://"
        size_t end = announce.find('/', start);
        _tracker_hostname = announce.substr(start, end - start);

        // Check for port in host (e.g., "host:8080")
        size_t colon = _tracker_hostname.find(':');
        if (colon != std::string::npos)
        {
            _tracker_port = std::stoi(_tracker_hostname.substr(colon + 1));
            _tracker_hostname = _tracker_hostname.substr(0, colon);
        }

        // Check for path in announce (e.g.,
        // "https://torrent.ubuntu.com/announce/path")
        size_t path_start = announce.find('/', end);
        if (path_start != std::string::npos)
        {
            _tracker_path = announce.substr(path_start);
        }
    }

    if (_tracker_port == 0)
    {
        if (announce.find("https://") == 0)
        {
            _tracker_port = 443;
        }
        else if (announce.find("http://") == 0)
        {
            _tracker_port = 80;
        }
        else
        {
            throw std::runtime_error("Invalid announce URL: " + announce);
        }
    }

    LOG_D("Tracker hostname: " + _tracker_hostname + ", Tracker port: " +
          std::to_string(_tracker_port) + ", Tracker path: " + _tracker_path);
}