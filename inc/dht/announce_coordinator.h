#pragma once

#include <chrono>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "info_hash.h"
#include "peer_address.h"

namespace dht
{

// One outbound announce_peer the DHT client should send.
struct AnnounceRequest
{
    PeerAddress pa;
    InfoHash info_hash;
    uint16_t listen_port;
    std::string token;
};

// Tracks torrents we are seeding on the DHT and tokens received from remote
// get_peers responses. Does not perform UDP I/O; DhtClient sends requests.
class AnnounceCoordinator
{
   public:
    void register_torrent(InfoHash info_hash, uint16_t port);
    void unregister_torrent(InfoHash info_hash);
    void clear();

    // Cache token from a get_peers response and announce immediately if the
    // info hash is registered. Appends zero or one request to out.
    void on_get_peers_response(InfoHash info_hash,
                               PeerAddress pa,
                               const std::string& token,
                               std::vector<AnnounceRequest>& out);

    // Announce to every node that has returned a token.
    void collect_announce_all(InfoHash info_hash,
                              uint16_t port,
                              std::vector<AnnounceRequest>& out) const;

    // Registered torrents due for a get_peers refresh.
    std::vector<InfoHash> torrents_needing_refresh(
        std::chrono::steady_clock::time_point now) const;

    void mark_refreshed(InfoHash info_hash,
                        std::chrono::steady_clock::time_point now);

    // Torrents registered before the routing table had nodes; retry get_peers
    // once candidates exist.
    std::vector<InfoHash> torrents_needing_initial_lookup() const;

    void mark_initial_lookup_started(InfoHash info_hash);

   private:
    struct RegisteredTorrent
    {
        uint16_t listen_port;
        std::chrono::steady_clock::time_point last_refresh;
        bool initial_lookup_pending{false};
    };

    mutable std::mutex mutex_;
    std::unordered_map<InfoHash, RegisteredTorrent> registered_;
    std::unordered_map<PeerAddress, std::string> received_tokens_;
};

}  // namespace dht
