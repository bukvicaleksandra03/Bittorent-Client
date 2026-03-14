#pragma once

#include <random>

#include "socket.h"
#include "tracker_request.h"

// UDP Tracker Protocol (BEP 15)
// https://www.bittorrent.org/beps/bep_0015.html
class TrackerRequestUDP : public TrackerRequest
{
   public:
    TrackerRequestUDP(const PeerId& peer_id,
                      const std::unique_ptr<TorrentFile>& tf);

    TrackerResponse send() override;

   private:
    static constexpr uint64_t PROTOCOL_ID = 0x41727101980ULL;
    static constexpr uint32_t ACTION_CONNECT = 0;
    static constexpr uint32_t ACTION_ANNOUNCE = 1;
    static constexpr uint32_t ACTION_ERROR = 3;
    static constexpr int TIMEOUT_MS = 15000;
    static constexpr int MAX_RETRIES = 3;

    enum class Event : uint32_t
    {
        None = 0,
        Completed = 1,
        Started = 2,
        Stopped = 3
    };

    std::mt19937 _rng{std::random_device{}()};

    uint32_t generate_transaction_id();

    // UDP connect phase - returns connection_id
    uint64_t udp_connect(Socket& socket, Address& addr);

    // UDP announce phase - returns response
    TrackerResponse udp_announce(Socket& socket,
                                 Address& addr,
                                 uint64_t connection_id);

    // Send packet and receive response with timeout
    ssize_t send_recv(Socket& socket,
                      Address& addr,
                      const uint8_t* send_buf,
                      size_t send_len,
                      uint8_t* recv_buf,
                      size_t recv_len,
                      int timeout_ms);
};
