// tests/dht/test_dht_loopback.cpp
// DHT tests that exercise real UDP I/O.
//
// LocalPingPopulatesRoutingTable — localhost only, no public routers required.
// BootstrapPopulatesTable — hits public bootstrap nodes (network-dependent).

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "dht/dht_node.h"
#include "dht/krpc.h"
#include "dht/node_id.h"
#include "net/socket.h"
#include "net/socket_addresses.h"

namespace
{

int next_loopback_port()
{
    static std::atomic<int> port_counter{55200};
    return ++port_counter;
}

bool wait_for_routing_table(dht::DhtNode& node,
                            size_t min_size,
                            std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (node.routing_table_size() >= min_size)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return node.routing_table_size() >= min_size;
}

}  // namespace

TEST(DhtNode, LocalPingPopulatesRoutingTable)
{
    const uint16_t port = static_cast<uint16_t>(next_loopback_port());

    dht::DhtNode server(port);
    server.start();
    ASSERT_TRUE(server.is_running());

    dht::NodeId client_id = dht::NodeId::random();
    UDPSocket client(AF_INET);
    IPv4Address dest("127.0.0.1", port);
    client.sendto(dht::make_ping(dht::random_txn(), client_id), dest);

    EXPECT_TRUE(wait_for_routing_table(server, 1, std::chrono::seconds(2)))
        << "expected ping sender in routing table";

    server.stop();
}

TEST(DhtNode, BootstrapPopulatesTable)
{
    dht::DhtNode node(0);
    node.start();
    std::this_thread::sleep_for(std::chrono::seconds(5));
    EXPECT_GT(node.routing_table_size(), 0u);
    node.stop();
}
