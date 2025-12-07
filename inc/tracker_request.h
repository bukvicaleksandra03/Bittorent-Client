#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "crypto.h"
#include "torrent_file.h"

class TrackerRequest
{
   public:
    TrackerRequest(const std::unique_ptr<TorrentFile>& tf);
    ~TrackerRequest();

    void send();
    void receive();

    // Build the HTTP request string (for debugging/testing)
    std::string build_request() const;

   private:
    crypto::SHA1Hash _info_hash_raw;
    std::string _peer_id;
    uint32_t _peer_ip;
    uint16_t _peer_port;
    uint64_t _downloaded;
    uint64_t _left;
    uint64_t _uploaded;
    uint32_t _event;

    std::string _tracker_hostname;
    uint16_t _tracker_port;
    std::string _tracker_path;
    void extract_host_name_port_and_path(const std::string& announce);
};