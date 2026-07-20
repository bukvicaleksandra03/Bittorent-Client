#include "dht/routing_table.h"

#include <arpa/inet.h>

#include <algorithm>
#include <cstring>

namespace dht
{

// ---------------------------------------------------------------------------
// RoutingEntry helpers
// ---------------------------------------------------------------------------

static constexpr auto NODE_GOOD_TIMEOUT = std::chrono::minutes(15);

bool RoutingEntry::is_good() const
{
    auto age = std::chrono::steady_clock::now() - last_seen;
    return age < NODE_GOOD_TIMEOUT;
}

std::string RoutingEntry::compact() const
{
    std::string out(26, '\0');
    // 20-byte node ID
    std::memcpy(out.data(), id.bytes.data(), 20);
    // 4-byte IPv4 address (network byte order)
    struct in_addr addr
    {
    };
    inet_aton(ip.c_str(), &addr);
    std::memcpy(out.data() + 20, &addr.s_addr, 4);
    // 2-byte port (network byte order)
    uint16_t port_be = htons(port);
    std::memcpy(out.data() + 24, &port_be, 2);
    return out;
}

RoutingEntry RoutingEntry::from_compact(const std::string& data, size_t offset)
{
    RoutingEntry entry;
    if (data.size() < offset + 26)
        return entry;

    entry.id = NodeId::from_string(data.substr(offset, 20));

    uint32_t ip_raw{};
    std::memcpy(&ip_raw, data.data() + offset + 20, 4);
    struct in_addr addr
    {
    };
    addr.s_addr = ip_raw;
    entry.ip = inet_ntoa(addr);

    uint16_t port_be{};
    std::memcpy(&port_be, data.data() + offset + 24, 2);
    entry.port = ntohs(port_be);

    entry.last_seen = std::chrono::steady_clock::now();
    return entry;
}

// ---------------------------------------------------------------------------
// RoutingTable
// ---------------------------------------------------------------------------

RoutingTable::RoutingTable(const NodeId& self_id, size_t max_size)
    : self_id_(self_id), max_size_(max_size)
{
}

void RoutingTable::add(const RoutingEntry& entry)
{
    if (entry.id == self_id_)
        return;

    // Update existing entry if present.
    for (auto& n : entries_)
    {
        if (n.id == entry.id)
        {
            n = entry;
            return;
        }
    }

    if (entries_.size() < max_size_)
    {
        entries_.push_back(entry);
        return;
    }

    // Table is full: replace the furthest entry if the new one is closer.
    NodeId new_dist = self_id_ ^ entry.id;
    size_t worst_idx = 0;
    NodeId worst_dist = self_id_ ^ entries_[0].id;

    for (size_t i = 1; i < entries_.size(); ++i)
    {
        NodeId d = self_id_ ^ entries_[i].id;
        if (worst_dist < d)
        {
            worst_dist = d;
            worst_idx = i;
        }
    }

    if (new_dist < worst_dist)
        entries_[worst_idx] = entry;
}

void RoutingTable::remove(const NodeId& id)
{
    entries_.erase(
        std::remove_if(entries_.begin(),
                       entries_.end(),
                       [&id](const RoutingEntry& n) { return n.id == id; }),
        entries_.end());
}

std::vector<RoutingEntry> RoutingTable::closest(const NodeId& target,
                                                size_t k) const
{
    std::vector<RoutingEntry> sorted = entries_;
    std::sort(sorted.begin(),
              sorted.end(),
              [&target](const RoutingEntry& a, const RoutingEntry& b)
              { return (target ^ a.id) < (target ^ b.id); });

    if (sorted.size() > k)
        sorted.resize(k);
    return sorted;
}

}  // namespace dht
