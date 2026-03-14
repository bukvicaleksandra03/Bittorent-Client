#include "tracker_request_http.h"

#include "http_request.h"
#include "logger.h"
#include "socket.h"
#include "ssl_socket.h"

TrackerRequestHTTP::TrackerRequestHTTP(const PeerId& peer_id,
                                       const std::unique_ptr<TorrentFile>& tf)
    : TrackerRequest(peer_id, tf)
{
    _tracker_port = tf->get_tracker_port();
    if (_tracker_port == 0)
    {
        if (_protocol == TrackerProtocol::HTTPS)
        {
            _tracker_port = 443;
        }
        else
        {
            _tracker_port = 80;
        }
    }
}

std::string TrackerRequestHTTP::build_http_request() const
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
        .query("numwant", "50")
        .query("event", "started")
        .header("User-Agent", "BitTorrent-Client/1.0")
        .header("Accept", "*/*")
        .build();
}

TrackerResponse TrackerRequestHTTP::send()
{
    LOG_D("Sending HTTP tracker request to " + _tracker_hostname + ":" +
          std::to_string(_tracker_port));

    // DNS lookup
    auto addresses = dns_lookup(
        _tracker_hostname, std::to_string(_tracker_port), SOCK_STREAM);

    if (addresses.empty())
    {
        throw std::runtime_error("Failed to resolve tracker: " +
                                 _tracker_hostname);
    }

    std::string request = build_http_request();
    LOG_D("HTTP Request:\n" + request);

    // Try each resolved address
    for (const auto& address : addresses)
    {
        try
        {
            LOG_D("Trying address: " + address->identifier);

            Socket socket(address->domain(), SOCK_STREAM);
            socket.connect_with_timeout(*address, 10000);

            std::string response;

            if (_protocol == TrackerProtocol::HTTPS)
            {
                // Use SSL socket for HTTPS
                SSLSocket ssl_socket(std::move(socket), _tracker_hostname);
                ssl_socket.send(request.c_str(), request.size());
                response = ssl_socket.recv_all();
            }
            else
            {
                // Plain HTTP
                socket.set_timeout(30);
                socket.send(request.c_str(), request.size());
                response = socket.recv_all();
            }

            LOG_D("Received response (" + std::to_string(response.size()) +
                  " bytes)");

            return TrackerResponseHTTP(response);
        }
        catch (const std::exception& e)
        {
            LOG_D("Failed: " + std::string(e.what()));
        }
    }

    throw std::runtime_error("Failed to connect to tracker");
}
