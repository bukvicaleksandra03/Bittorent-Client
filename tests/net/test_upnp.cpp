// tests/net/test_upnp.cpp
//
// Integration tests for UPnPPortMapping.
//
// These tests talk to a real router, so they are network-dependent.
// When no UPnP gateway is present on the LAN the tests skip gracefully
// via GTEST_SKIP() instead of failing.
//
// Run with:  make test   (or ./build/test_upnp directly)

#include <gtest/gtest.h>

#include <cstdint>
#include <iostream>

#include "net/upnp.h"

// All tests use ports in the 19880–19889 range to avoid clashing with any
// real service while still being in the unprivileged range.

class UPnPTest : public ::testing::Test
{
   protected:
    static constexpr uint16_t PORT_A = 19881;
    static constexpr uint16_t PORT_B = 19882;
    static constexpr uint16_t PORT_C = 19883;
};

// ---------------------------------------------------------------------------
// Basic creation
// ---------------------------------------------------------------------------

// create() must return either a valid optional or nullopt – it must never
// throw or crash, regardless of what the router does.
TEST_F(UPnPTest, CreateReturnsValueOrNullopt)
{
    auto m = UPnPPortMapping::create(PORT_A);
    // Result is either engaged (mapping obtained) or disengaged (no gateway).
    // Both outcomes are acceptable here; we just verify the call completes.
    SUCCEED();
}

// When create() succeeds, external_port() must be non-zero.
TEST_F(UPnPTest, ExternalPortIsNonZeroOnSuccess)
{
    auto m = UPnPPortMapping::create(PORT_A);
    if (!m)
        GTEST_SKIP() << "No UPnP gateway found – test skipped";

    EXPECT_GT(m->external_port(), 0u);
}

// When create() succeeds, external_ip() must be a non-empty string.
TEST_F(UPnPTest, ExternalIpIsNonEmptyOnSuccess)
{
    auto m = UPnPPortMapping::create(PORT_A);
    if (!m)
        GTEST_SKIP() << "No UPnP gateway found – test skipped";

    EXPECT_FALSE(m->external_ip().empty());

    std::cout << "[UPnP] external_ip  = " << m->external_ip() << '\n';
    std::cout << "[UPnP] external_port= " << m->external_port() << '\n';
}

// ---------------------------------------------------------------------------
// RAII / destructor
// ---------------------------------------------------------------------------

// The destructor must send DeletePortMapping and must not crash.
// If it crashes (or raises SIGABRT / SIGSEGV) the test process will die and
// the test will be reported as failed.
TEST_F(UPnPTest, DestructorReleasesMapping)
{
    {
        auto m = UPnPPortMapping::create(PORT_A);
        if (!m)
            GTEST_SKIP() << "No UPnP gateway found – test skipped";

        EXPECT_GT(m->external_port(), 0u);
        // m goes out of scope → destructor fires → DeletePortMapping sent.
    }
    SUCCEED();
}

// Creating a second mapping on a different port and then destroying it should
// also work without crashing (exercises the release path twice in one test).
TEST_F(UPnPTest, DestructorCalledTwiceIndependently)
{
    auto m1 = UPnPPortMapping::create(PORT_A);
    auto m2 = UPnPPortMapping::create(PORT_B);

    if (!m1 && !m2)
        GTEST_SKIP() << "No UPnP gateway found – test skipped";

    // Destroy m2 explicitly before m1 to test ordering.
    m2.reset();
    m1.reset();

    SUCCEED();
}

// ---------------------------------------------------------------------------
// Move semantics
// ---------------------------------------------------------------------------

// After move-construction the new object must carry the original values,
// and its destructor (not the old object's) must release the mapping.
TEST_F(UPnPTest, MoveConstructorPreservesValues)
{
    auto opt = UPnPPortMapping::create(PORT_A);
    if (!opt)
        GTEST_SKIP() << "No UPnP gateway found – test skipped";

    const uint16_t    port = opt->external_port();
    const std::string ip   = opt->external_ip();

    // Move the value out of the optional.
    UPnPPortMapping moved(std::move(*opt));

    EXPECT_EQ(moved.external_port(), port);
    EXPECT_EQ(moved.external_ip(),   ip);
    EXPECT_GT(moved.external_port(), 0u);
    // 'moved' now owns the mapping; its destructor will call DeletePortMapping.
}

// Move-assignment must similarly transfer ownership without a double-delete.
TEST_F(UPnPTest, MoveAssignmentPreservesValues)
{
    auto src = UPnPPortMapping::create(PORT_A);
    if (!src)
        GTEST_SKIP() << "No UPnP gateway found – test skipped";

    const uint16_t    port = src->external_port();
    const std::string ip   = src->external_ip();

    std::optional<UPnPPortMapping> dst;
    dst = std::move(src);  // move the optional itself

    ASSERT_TRUE(dst.has_value());
    EXPECT_EQ(dst->external_port(), port);
    EXPECT_EQ(dst->external_ip(),   ip);
}

// ---------------------------------------------------------------------------
// Optional external_port override
// ---------------------------------------------------------------------------

// When an explicit external port is requested the router may or may not
// honour it, but the call must not crash and – if it succeeds – the returned
// external port must be non-zero.
TEST_F(UPnPTest, ExplicitExternalPortRequest)
{
    auto m = UPnPPortMapping::create(PORT_A, PORT_C);
    if (!m)
        GTEST_SKIP() << "No UPnP gateway found – test skipped";

    EXPECT_GT(m->external_port(), 0u);
    std::cout << "[UPnP] requested external=" << PORT_C
              << "  got=" << m->external_port() << '\n';
}
