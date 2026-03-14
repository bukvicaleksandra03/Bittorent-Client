#include "tracker_response.h"

#include "bencode_parser.h"
#include "bencode_types.h"
#include "logger.h"

// ============================================================================
// TrackerResponseHTTP
// ============================================================================

TrackerResponseHTTP::TrackerResponseHTTP(const std::string& http_response)
{
    std::string body = extract_body(http_response);

    if (body.empty())
    {
        _failure_reason = "Empty response body";
        return;
    }

    parse_bencode(body);
}

std::string TrackerResponseHTTP::extract_body(const std::string& http_response)
{
    // Find end of HTTP headers
    size_t pos = http_response.find("\r\n\r\n");
    if (pos != std::string::npos)
    {
        return http_response.substr(pos + 4);
    }

    pos = http_response.find("\n\n");
    if (pos != std::string::npos)
    {
        return http_response.substr(pos + 2);
    }

    return "";
}

void TrackerResponseHTTP::parse_bencode(const std::string& body)
{
    try
    {
        BencodeParser parser(body, true);
        auto root = parser.parse_value();

        if (!root || root->type() != BType::Type::Dictionary)
        {
            _failure_reason = "Invalid bencode response";
            return;
        }

        auto* dict = dynamic_cast<BDict*>(root.get());

        // Check for failure
        if (dict->has_key("failure reason"))
        {
            _failure_reason = as<BString>((*dict)["failure reason"])->content;
            return;
        }

        // Parse fields
        if (dict->has_key("interval"))
        {
            _interval =
                static_cast<int32_t>(as<BInteger>((*dict)["interval"])->value);
        }

        if (dict->has_key("complete"))
        {
            _seeders =
                static_cast<int32_t>(as<BInteger>((*dict)["complete"])->value);
        }

        if (dict->has_key("incomplete"))
        {
            _leechers = static_cast<int32_t>(
                as<BInteger>((*dict)["incomplete"])->value);
        }

        // Parse peers
        if (dict->has_key("peers"))
        {
            auto* peers_val = (*dict)["peers"];

            if (peers_val->type() == BType::Type::String)
            {
                // Compact format
                _peers = parse_compact_peers(as<BString>(peers_val)->content);
            }
            else if (peers_val->type() == BType::Type::List)
            {
                // Dictionary format
                auto* peer_list = as<BList>(peers_val);
                for (const auto& entry : peer_list->content)
                {
                    if (entry->type() == BType::Type::Dictionary)
                    {
                        auto* pd = dynamic_cast<BDict*>(entry.get());
                        Peer p;
                        if (pd->has_key("ip"))
                        {
                            p.ip = as<BString>((*pd)["ip"])->content;
                        }
                        if (pd->has_key("port"))
                        {
                            p.port = static_cast<uint16_t>(
                                as<BInteger>((*pd)["port"])->value);
                        }
                        _peers.push_back(p);
                    }
                }
            }
        }

        LOG_D("Parsed HTTP response: " + std::to_string(_peers.size()) +
              " peers");
    }
    catch (const std::exception& e)
    {
        _failure_reason = std::string("Parse error: ") + e.what();
    }
}

// ============================================================================
// TrackerResponseUDP
// ============================================================================

TrackerResponseUDP::TrackerResponseUDP(int32_t interval,
                                       int32_t leechers,
                                       int32_t seeders,
                                       std::vector<Peer> peers)
{
    _interval = interval;
    _leechers = leechers;
    _seeders = seeders;
    _peers = std::move(peers);
}

TrackerResponseUDP TrackerResponseUDP::failure(const std::string& reason)
{
    TrackerResponseUDP resp;
    resp._failure_reason = reason;
    return resp;
}
