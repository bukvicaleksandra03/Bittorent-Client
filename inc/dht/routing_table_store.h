#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "dht/node_id.h"
#include "dht/routing_table.h"

namespace dht
{

// Persistence for DHT routing table (BEP 5 k-buckets).
//
// File format (text, version 1):
//   version=1
//   self_id=<40 hex chars>          (informational; not required to match on load)
//   entry=<node_id_hex> <ip> <port>
//
// Default path: routing_table_store/routing_table.txt
// Override with DHT_ROUTING_TABLE_PATH or DhtClient::set_routing_table_path().
struct RoutingTableSnapshot
{
    NodeId self_id;
    std::vector<RoutingEntry> entries;
};

struct RoutingTableStoreResult
{
    bool ok{false};
    std::string error;
    RoutingTableSnapshot snapshot;
};

// Minimum verified-good entries after load to skip public bootstrap on start().
// DhtClient pings loaded peers and counts only those that respond.
static constexpr size_t kMinGoodEntriesToSkipBootstrap = 4;

class RoutingTableStore
{
   public:
    static RoutingTableStoreResult load(const std::string& path);

    // Writes all entries from table; self_id is stored for debugging only.
    static bool save(const RoutingTable& table,
                     const NodeId& self_id,
                     const std::string& path,
                     std::string* error_out = nullptr);

    static bool remove_file(const std::string& path,
                            std::string* error_out = nullptr);

    static std::string default_path();
};

}  // namespace dht
