#pragma once

#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

#include "peer.h"

// Base class for tracker responses
class TrackerResponse
{
   public:
    TrackerResponse() = default;
    virtual ~TrackerResponse() = default;

    // Getters
    int32_t get_interval() const
    {
        return _interval;
    }
    int32_t get_seeders() const
    {
        return _seeders;
    }
    int32_t get_leechers() const
    {
        return _leechers;
    }
    const std::vector<Peer>& get_peers() const
    {
        return _peers;
    }
    const std::string& get_failure_reason() const
    {
        return _failure_reason;
    }
    bool has_failed() const
    {
        return !_failure_reason.empty();
    }

    // Convert to string for printing
    std::string to_string() const
    {
        std::string result;
        if (has_failed())
        {
            result = "TrackerResponse [FAILED]\n";
            result += "  Failure reason: " + _failure_reason + "\n";
        }
        else
        {
            result = "TrackerResponse [OK]\n";
            result += "  Interval: " + std::to_string(_interval) + "s\n";
            result += "  Seeders:  " + std::to_string(_seeders) + "\n";
            result += "  Leechers: " + std::to_string(_leechers) + "\n";
            result += "  Peers:    " + std::to_string(_peers.size()) + "\n";
            for (const auto& peer : _peers)
            {
                result += "    - " + peer.to_string() + "\n";
            }
        }
        return result;
    }

    friend std::ostream& operator<<(std::ostream& os,
                                    const TrackerResponse& response)
    {
        return os << response.to_string();
    }

   protected:
    int32_t _interval = 0;
    int32_t _seeders = 0;
    int32_t _leechers = 0;
    std::vector<Peer> _peers;
    std::string _failure_reason;
};

// HTTP/HTTPS Tracker Response (parses bencode from HTTP body)
class TrackerResponseHTTP : public TrackerResponse
{
   public:
    explicit TrackerResponseHTTP(const std::string& http_response);

   private:
    std::string extract_body(const std::string& http_response);
    void parse_bencode(const std::string& body);
};

// UDP Tracker Response (constructed from parsed binary data)
class TrackerResponseUDP : public TrackerResponse
{
   public:
    TrackerResponseUDP(int32_t interval,
                       int32_t leechers,
                       int32_t seeders,
                       std::vector<Peer> peers);

    static TrackerResponseUDP failure(const std::string& reason);

   private:
    TrackerResponseUDP() = default;
};
