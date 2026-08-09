#include "trackers/http_tracker_communicator.h"

#include "trackers/tracker_scrape_stats.h"

#include <sstream>
#include <stdexcept>
#include <string_view>

#include "bencode/bencode_parser.h"
#include "crypto.h"
#include "logger.h"
#include "net/ssl_socket.h"
#include "peer_address.h"
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

static std::string build_query_string(const InfoHash& info_hash,
                                      const utils::PeerId& my_peer_id,
                                      uint64_t downloaded,
                                      uint64_t left,
                                      uint64_t uploaded,
                                      uint32_t event,
                                      uint16_t port)
{
    std::ostringstream qs;
    qs << "info_hash=" << info_hash.url_encoded();
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
        throw std::runtime_error(
            "Malformed HTTP response: no header/body separator");
    }

    std::string status_line = raw.substr(0, raw.find("\r\n"));
    size_t space1 = status_line.find(' ');
    if (space1 == std::string::npos)
    {
        throw std::runtime_error("Malformed HTTP status line");
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

static std::vector<PeerAddress> parse_tracker_response(
    const std::string& body, logger::Logger* log)
{
    bencode::Parser parser(reinterpret_cast<const uint8_t*>(body.data()),
                         body.size());
    auto root = parser.parse_value();

    BDict* dict = as<BDict>(root.get());

    if (dict->has_key("failure reason"))
    {
        std::string reason = dict->get_val<BString>("failure reason")->content;
        throw std::runtime_error("Tracker returned failure: " + reason);
    }

    if (dict->has_key("interval"))
    {
        int64_t interval = dict->get_val<BInteger>("interval")->value;
        if (log)
            LLOG_INFO(*log, "Tracker interval: " + std::to_string(interval) +
                                "s");
    }

    std::vector<PeerAddress> peers;

    if (!dict->has_key("peers"))
    {
        if (log)
            LLOG_WARNING(*log, "Tracker response has no 'peers' key");
        return peers;
    }

    BType* peers_val = (*dict)["peers"];

    if (peers_val->type() == BType::Type::String)
    {
        const std::string& compact = as<BString>(peers_val)->content;
        peers = parse_compact_peers(compact);
        if (log)
            LLOG_INFO(*log, "Parsed " + std::to_string(peers.size()) +
                                " peers from compact format");
    }
    else if (peers_val->type() == BType::Type::List)
    {
        BList* peer_list = as<BList>(peers_val);
        for (const auto& entry : peer_list->content)
        {
            BDict* pd = as<BDict>(entry.get());
            PeerAddress p;
            p.ip = pd->get_val<BString>("ip")->content;
            p.port =
                static_cast<uint16_t>(pd->get_val<BInteger>("port")->value);
            peers.push_back(p);
        }
        if (log)
            LLOG_INFO(*log, "Parsed " + std::to_string(peers.size()) +
                                " peers from dictionary format");
    }
    else
    {
        throw std::runtime_error("Unexpected type for 'peers' field");
    }

    return peers;
}

static std::string send_http_tracker_get(const TrackerDetails& tracker,
                                         const std::string& query_string,
                                         logger::Logger* log)
{
    const std::string request = build_http_request(tracker, query_string);

    if (log)
    {
        LLOG_DEBUG(*log, "Request:\n" + request);
    }

    auto addresses = dns_lookup(tracker.hostname,
                              std::to_string(tracker.port), SOCK_STREAM);

    if (addresses.empty())
    {
        throw std::runtime_error("DNS lookup failed for " + tracker.hostname);
    }

    const bool use_tls = (tracker.protocol == TrackerProtocol::HTTPS);
    constexpr int CONNECT_TIMEOUT_MS = 10000;
    constexpr int RECV_TIMEOUT_MS = 15000;

    for (auto& addr : addresses)
    {
        try
        {
            if (log)
            {
                LLOG_DEBUG(*log, "Trying address: " + addr->identifier);
            }

            const int domain =
                (addr->domain() == AF_INET6) ? AF_INET6 : AF_INET;
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
                if (log)
                {
                    LLOG_WARNING(*log, "Empty response from tracker");
                }
                continue;
            }

            if (log)
            {
                LLOG_DEBUG(*log,
                           "Received " +
                               std::to_string(raw_response.size()) +
                               " bytes from tracker");
            }

            HttpResponse http_resp = parse_http_response(raw_response);

            if (http_resp.status_code != 200)
            {
                if (log)
                {
                    LLOG_WARNING(*log,
                                 "Tracker returned HTTP " +
                                     std::to_string(http_resp.status_code));
                }
                continue;
            }

            return http_resp.body;
        }
        catch (const std::exception& e)
        {
            if (log)
            {
                LLOG_WARNING(*log, "Failed on address " + addr->identifier +
                                       ": " + e.what());
            }
        }
    }

    throw std::runtime_error(
        "HTTP tracker request failed: exhausted all addresses for " +
        tracker.hostname);
}

std::vector<PeerAddress> HTTPTrackerCommunicator::announce(
    const TrackerDetails& tracker,
    const InfoHash& info_hash,
    const utils::PeerId& my_peer_id,
    uint64_t downloaded,
    uint64_t left,
    uint64_t uploaded,
    uint32_t event,
    uint16_t port)
{
    const std::string query = build_query_string(
        info_hash, my_peer_id, downloaded, left, uploaded, event, port);

    if (m_logger)
    {
        LLOG_INFO(*m_logger, "HTTP announce to " + tracker.to_string());
    }

    const std::string body =
        send_http_tracker_get(tracker, query, m_logger.get());

    return parse_tracker_response(body, m_logger.get());
}

// ---------------------------------------------------------------------------
// HTTP tracker scrape
// ---------------------------------------------------------------------------

static std::string build_scrape_query_string(const InfoHash& info_hash)
{
    std::ostringstream oss;
    oss << "info_hash=" << info_hash.url_encoded();
    return oss.str();
}

static std::string announce_path_to_scrape_path(const std::string& path)
{
    constexpr std::string_view announce_suffix = "/announce";
    constexpr std::string_view scrape_suffix = "/scrape";

    if (path.size() >= announce_suffix.size() &&
        path.compare(path.size() - announce_suffix.size(),
                     announce_suffix.size(), announce_suffix) == 0)
    {
        return path.substr(0, path.size() - announce_suffix.size()) +
               std::string(scrape_suffix);
    }

    const auto pos = path.find("/announce");
    if (pos != std::string::npos)
    {
        std::string result = path;
        result.replace(pos, announce_suffix.size(), scrape_suffix);
        return result;
    }

    if (path == "announce")
    {
        return "scrape";
    }

    return std::string(scrape_suffix);
}

TrackerDetails scrape_tracker_from_announce(const TrackerDetails& announce)
{
    TrackerDetails scrape = announce;
    scrape.path = announce_path_to_scrape_path(announce.path);
    return scrape;
}

TrackerScrapeStats parse_http_scrape_response(const std::string& body,
                                              const InfoHash& info_hash)
{
    TrackerScrapeStats stats;

    bencode::Parser parser(reinterpret_cast<const uint8_t*>(body.data()),
                         body.size());
    auto root = parser.parse_value();
    BDict* dict = as<BDict>(root.get());

    if (dict->has_key("failure reason"))
    {
        throw std::runtime_error("Tracker scrape failure: " +
                                 dict->get_val<BString>("failure reason")
                                     ->content);
    }

    if (!dict->has_key("files"))
    {
        return stats;
    }

    BDict* files = as<BDict>((*dict)["files"]);
    const std::string hash_key = info_hash.to_raw();

    if (!files->has_key(hash_key))
    {
        return stats;
    }

    BDict* file_stats = as<BDict>((*files)[hash_key]);

    if (file_stats->has_key("complete"))
    {
        stats.complete =
            static_cast<uint64_t>(file_stats->get_val<BInteger>("complete")
                                      ->value);
    }

    if (file_stats->has_key("incomplete"))
    {
        stats.incomplete =
            static_cast<uint64_t>(file_stats->get_val<BInteger>("incomplete")
                                      ->value);
    }

    if (file_stats->has_key("downloaded"))
    {
        stats.downloaded =
            static_cast<uint64_t>(file_stats->get_val<BInteger>("downloaded")
                                      ->value);
    }
    else if (file_stats->has_key("download"))
    {
        stats.downloaded =
            static_cast<uint64_t>(file_stats->get_val<BInteger>("download")
                                      ->value);
    }

    return stats;
}

TrackerScrapeStats HTTPTrackerCommunicator::scrape(
    const TrackerDetails& tracker, const InfoHash& info_hash)
{
    const TrackerDetails scrape_tracker = scrape_tracker_from_announce(tracker);
    const std::string query = build_scrape_query_string(info_hash);

    if (m_logger)
    {
        LLOG_INFO(*m_logger,
                  "Scraping HTTP tracker: " + scrape_tracker.to_string());
    }

    const std::string body =
        send_http_tracker_get(scrape_tracker, query, m_logger.get());

    return parse_http_scrape_response(body, info_hash);
}
