#include "trackers/http_tracker_communicator.h"

#include <sstream>

#include "bencode/bencode_parser.h"
#include "crypto.h"
#include "logger.h"
#include "net/ssl_socket.h"
#include "peer.h"
#include "trackers/tracker_details.h"
#include "trackers/tracker_protocol.h"
#include "utils.h"

static std::string event_to_string(uint32_t event)
{
    switch (event)
    {
        case 1:
            return "completed";
        case 2:
            return "started";
        case 3:
            return "stopped";
        default:
            return "";
    }
}

static std::string build_query_string(const crypto::SHA1Hash& info_hash,
                                      const utils::PeerId& my_peer_id,
                                      uint64_t downloaded,
                                      uint64_t left,
                                      uint64_t uploaded,
                                      uint32_t event,
                                      uint16_t port)
{
    std::ostringstream qs;
    qs << "info_hash=" << crypto::url_encode(info_hash);
    qs << "&peer_id=" << crypto::url_encode(my_peer_id);
    qs << "&port=" << port;
    qs << "&uploaded=" << uploaded;
    qs << "&downloaded=" << downloaded;
    qs << "&left=" << left;
    qs << "&compact=1";

    std::string ev = event_to_string(event);
    if (!ev.empty())
    {
        qs << "&event=" << ev;
    }

    return qs.str();
}

static std::string build_host_header(const TrackerDetails& tracker)
{
    bool is_default_port =
        (tracker.protocol == TrackerProtocol::HTTP && tracker.port == 80) ||
        (tracker.protocol == TrackerProtocol::HTTPS && tracker.port == 443);

    if (is_default_port)
    {
        return tracker.hostname;
    }
    return tracker.hostname + ":" + std::to_string(tracker.port);
}

static std::string build_http_request(const TrackerDetails& tracker,
                                      const std::string& query_string)
{
    std::ostringstream req;
    req << "GET " << tracker.path << "?" << query_string << " HTTP/1.1\r\n";
    req << "Host: " << build_host_header(tracker) << "\r\n";
    req << "Connection: close\r\n";
    req << "Accept-Encoding: identity\r\n";
    req << "\r\n";
    return req.str();
}

struct HttpResponse
{
    int status_code = 0;
    std::string body;
};

static HttpResponse parse_http_response(const std::string& raw)
{
    HttpResponse resp;

    size_t header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos)
    {
        LOG_AND_THROW("Malformed HTTP response: no header/body separator");
    }

    std::string status_line = raw.substr(0, raw.find("\r\n"));
    size_t space1 = status_line.find(' ');
    if (space1 == std::string::npos)
    {
        LOG_AND_THROW("Malformed HTTP status line");
    }
    size_t space2 = status_line.find(' ', space1 + 1);
    if (space2 == std::string::npos)
    {
        space2 = status_line.size();
    }

    resp.status_code =
        std::stoi(status_line.substr(space1 + 1, space2 - space1 - 1));

    resp.body = raw.substr(header_end + 4);
    return resp;
}

static std::vector<Peer> parse_tracker_response(const std::string& body)
{
    BencodeParser parser(reinterpret_cast<const uint8_t*>(body.data()),
                         body.size());
    auto root = parser.parse_value();

    BDict* dict = as<BDict>(root.get());

    if (dict->has_key("failure reason"))
    {
        std::string reason = dict->get_val<BString>("failure reason")->content;
        LOG_AND_THROW("Tracker returned failure: " + reason);
    }

    if (dict->has_key("interval"))
    {
        int64_t interval = dict->get_val<BInteger>("interval")->value;
        LOG_I("Tracker interval: " + std::to_string(interval) + "s");
    }

    std::vector<Peer> peers;

    if (!dict->has_key("peers"))
    {
        LOG_W("Tracker response has no 'peers' key");
        return peers;
    }

    BType* peers_val = (*dict)["peers"];

    if (peers_val->type() == BType::Type::String)
    {
        const std::string& compact = as<BString>(peers_val)->content;
        peers = parse_compact_peers(compact);
        LOG_I("Parsed " + std::to_string(peers.size()) +
              " peers from compact format");
    }
    else if (peers_val->type() == BType::Type::List)
    {
        BList* peer_list = as<BList>(peers_val);
        for (const auto& entry : peer_list->content)
        {
            BDict* pd = as<BDict>(entry.get());
            Peer p;
            p.ip = pd->get_val<BString>("ip")->content;
            p.port =
                static_cast<uint16_t>(pd->get_val<BInteger>("port")->value);
            peers.push_back(p);
        }
        LOG_I("Parsed " + std::to_string(peers.size()) +
              " peers from dictionary format");
    }
    else
    {
        LOG_AND_THROW("Unexpected type for 'peers' field");
    }

    return peers;
}

std::vector<Peer> HTTPTrackerCommunicator::announce(
    const TrackerDetails& tracker,
    const crypto::SHA1Hash& info_hash,
    const utils::PeerId& my_peer_id,
    uint64_t downloaded,
    uint64_t left,
    uint64_t uploaded,
    uint32_t event,
    uint16_t port)
{
    bool use_tls = (tracker.protocol == TrackerProtocol::HTTPS);

    std::string query = build_query_string(info_hash, my_peer_id, downloaded,
                                           left, uploaded, event, port);
    std::string request = build_http_request(tracker, query);

    LOG_I("HTTP announce to " + tracker.to_string());
    LOG_D("Request:\n" + request);

    auto addresses = dns_lookup(tracker.hostname,
                                std::to_string(tracker.port), SOCK_STREAM);

    if (addresses.empty())
    {
        LOG_AND_THROW("DNS lookup failed for " + tracker.hostname);
    }

    constexpr int CONNECT_TIMEOUT_MS = 10000;
    constexpr int RECV_TIMEOUT_MS = 15000;

    for (auto& addr : addresses)
    {
        try
        {
            LOG_D("Trying address: " + addr->identifier);

            int domain = (addr->domain() == AF_INET6) ? AF_INET6 : AF_INET;
            TCPClientSocket tcp_sock(domain);
            tcp_sock.connect_with_timeout(*addr, CONNECT_TIMEOUT_MS);

            std::string raw_response;

            if (use_tls)
            {
                SSLSocket ssl_sock(std::move(tcp_sock), tracker.hostname);
                ssl_sock.send(request.c_str(), request.size());
                raw_response = ssl_sock.recv_all();
            }
            else
            {
                tcp_sock.send(request.c_str(), request.size());
                raw_response = tcp_sock.recv_all_with_timeout(RECV_TIMEOUT_MS);
            }

            if (raw_response.empty())
            {
                LOG_W("Empty response from tracker");
                continue;
            }

            LOG_D("Received " + std::to_string(raw_response.size()) +
                  " bytes from tracker");

            HttpResponse http_resp = parse_http_response(raw_response);

            if (http_resp.status_code != 200)
            {
                LOG_W("Tracker returned HTTP " +
                      std::to_string(http_resp.status_code));
                continue;
            }

            return parse_tracker_response(http_resp.body);
        }
        catch (const std::exception& e)
        {
            LOG_W("Failed on address " + addr->identifier + ": " + e.what());
            continue;
        }
    }

    LOG_AND_THROW("HTTP announce failed: exhausted all addresses for " +
                  tracker.hostname);
}
