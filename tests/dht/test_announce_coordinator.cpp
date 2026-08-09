#include "dht/announce_coordinator.h"

#include <gtest/gtest.h>

#include <chrono>

namespace dht
{
namespace
{

InfoHash make_hash(uint8_t b)
{
    InfoHash h{};
    h.bytes[0] = b;
    return h;
}

TEST(AnnounceCoordinatorTest, RegisterSetsInitialLookupPending)
{
    AnnounceCoordinator coord;
    const InfoHash hash = make_hash(0x4a);

    coord.register_torrent(hash, 6881);

    const auto pending = coord.torrents_needing_initial_lookup();
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_EQ(pending[0], hash);
}

TEST(AnnounceCoordinatorTest, MarkInitialLookupStartedClearsPending)
{
    AnnounceCoordinator coord;
    const InfoHash hash = make_hash(0x4a);

    coord.register_torrent(hash, 6881);
    coord.mark_initial_lookup_started(hash);

    EXPECT_TRUE(coord.torrents_needing_initial_lookup().empty());
}

TEST(AnnounceCoordinatorTest, RefreshIntervalIndependentOfInitialLookup)
{
    AnnounceCoordinator coord;
    const InfoHash hash = make_hash(0x4a);
    const auto now = std::chrono::steady_clock::now();

    coord.register_torrent(hash, 6881);
    EXPECT_TRUE(coord.torrents_needing_refresh(now).empty());

    coord.mark_initial_lookup_started(hash);
    EXPECT_TRUE(coord.torrents_needing_refresh(now).empty());
}

}  // namespace
}  // namespace dht
