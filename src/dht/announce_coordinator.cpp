#include "dht/announce_coordinator.h"

namespace dht
{

namespace
{

static constexpr auto kAnnounceRefreshInterval = std::chrono::minutes(12);

}  // namespace

void AnnounceCoordinator::register_torrent(InfoHash info_hash, uint16_t port)
{
    std::lock_guard<std::mutex> lk(mutex_);
    registered_[info_hash] =
        RegisteredTorrent{port, std::chrono::steady_clock::now()};
}

void AnnounceCoordinator::unregister_torrent(InfoHash info_hash)
{
    std::lock_guard<std::mutex> lk(mutex_);
    registered_.erase(info_hash);
}

void AnnounceCoordinator::clear()
{
    std::lock_guard<std::mutex> lk(mutex_);
    registered_.clear();
    received_tokens_.clear();
}

void AnnounceCoordinator::on_get_peers_response(
    InfoHash info_hash,
    PeerAddress pa,
    const std::string& token,
    std::vector<AnnounceRequest>& out)
{
    if (token.empty())
        return;

    std::lock_guard<std::mutex> lk(mutex_);
    received_tokens_[pa] = token;

    auto it = registered_.find(info_hash);
    if (it == registered_.end())
        return;

    out.push_back(
        AnnounceRequest{pa, info_hash, it->second.listen_port, token});
}

void AnnounceCoordinator::collect_announce_all(
    InfoHash info_hash,
    uint16_t port,
    std::vector<AnnounceRequest>& out) const
{
    std::lock_guard<std::mutex> lk(mutex_);
    for (const auto& [peer_addr, token] : received_tokens_)
        out.push_back(AnnounceRequest{peer_addr, info_hash, port, token});
}

std::vector<InfoHash> AnnounceCoordinator::torrents_needing_refresh(
    std::chrono::steady_clock::time_point now) const
{
    std::vector<InfoHash> to_refresh;
    std::lock_guard<std::mutex> lk(mutex_);
    for (const auto& [hash, reg] : registered_)
    {
        if (now - reg.last_refresh >= kAnnounceRefreshInterval)
            to_refresh.push_back(hash);
    }
    return to_refresh;
}

void AnnounceCoordinator::mark_refreshed(
    InfoHash info_hash,
    std::chrono::steady_clock::time_point now)
{
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = registered_.find(info_hash);
    if (it != registered_.end())
        it->second.last_refresh = now;
}

}  // namespace dht
