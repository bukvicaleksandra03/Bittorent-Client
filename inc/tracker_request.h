#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "crypto.h"
#include "socket_addresses.h"
#include "torrent_file.h"
#include "utils.h"

using PeerId = utils::PeerId;
class TrackerRequest
{
   public:
    TrackerRequest(const PeerId& peer_id,
                   const std::unique_ptr<TorrentFile>& tf);
    ~TrackerRequest();

    void send(const std::vector<std::unique_ptr<Address>>& addresses);
    void receive();

    std::string build_request() const;

    const std::string& get_tracker_hostname() const
    {
        return _tracker_hostname;
    }
    uint16_t get_tracker_port() const
    {
        return _tracker_port;
    }

   private:
    crypto::SHA1Hash _info_hash;
    PeerId _peer_id;
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