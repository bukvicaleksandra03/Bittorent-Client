#include "dht/routing_table.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cstring>

namespace dht
{

// ---------------------------------------------------------------------------
// Node helpers
// ---------------------------------------------------------------------------

static constexpr auto NODE_GOOD_TIMEOUT = std::chrono::minutes(15);

bool Node::is_good() const
{
    auto age = std::chrono::steady_clock::now() - last_seen;
    return age < NODE_GOOD_TIMEOUT;
}

std::string Node::compact() const
{
    std::string out(26, '\0');
    // 20-byte node ID
    std::memcpy(out.data(), id.bytes.data(), 20);
    // 4-byte IPv4 address (network byte order)
    struct in_addr addr{};
    inet_aton(ip.c_str(), &addr);
    std::memcpy(out.data() + 20, &addr.s_addr, 4);
    // 2-byte port (network byte order)
    uint16_t port_be = htons(port);
    std::memcpy(out.data() + 24, &port_be, 2);
    return out;
}

Node Node::from_compact(const std::string& data, size_t offset)
{
    Node n;
    if (data.size() < offset + 26)
        return n;

    n.id = NodeId::from_string(data.substr(offset, 20));

    uint32_t ip_raw{};
    std::memcpy(&ip_raw, data.data() + offset + 20, 4);
    struct in_addr addr{};
    addr.s_addr = ip_raw;
    n.ip = inet_ntoa(addr);

    uint16_t port_be{};
    std::memcpy(&port_be, data.data() + offset + 24, 2);
    n.port = ntohs(port_be);

    n.last_seen = std::chrono::steady_clock::now();
    return n;
}

// ---------------------------------------------------------------------------
// RoutingTable
// ---------------------------------------------------------------------------

RoutingTable::RoutingTable(const NodeId& self_id, size_t max_size)
    : self_id_(self_id), max_size_(max_size)
{}

void RoutingTable::add(const Node& node)
{
    if (node.id == self_id_)
        return;

    // Update existing entry if present.
    for (auto& n : nodes_)
    {
        if (n.id == node.id)
        {
            n = node;
            return;
        }
    }

    if (nodes_.size() < max_size_)
    {
        nodes_.push_back(node);
        return;
    }

    // Table is full: replace the furthest node if the new one is closer.
    NodeId new_dist  = self_id_ ^ node.id;
    size_t worst_idx = 0;
    NodeId worst_dist = self_id_ ^ nodes_[0].id;

    for (size_t i = 1; i < nodes_.size(); ++i)
    {
        NodeId d = self_id_ ^ nodes_[i].id;
        if (worst_dist < d)
        {
            worst_dist = d;
            worst_idx  = i;
        }
    }

    if (new_dist < worst_dist)
        nodes_[worst_idx] = node;
}

void RoutingTable::remove(const NodeId& id)
{
    nodes_.erase(
        std::remove_if(nodes_.begin(), nodes_.end(),
                       [&id](const Node& n) { return n.id == id; }),
        nodes_.end());
}

std::vector<Node> RoutingTable::closest(const NodeId& target, size_t k) const
{
    std::vector<Node> sorted = nodes_;
    std::sort(sorted.begin(), sorted.end(),
              [&target](const Node& a, const Node& b)
              { return (target ^ a.id) < (target ^ b.id); });

    if (sorted.size() > k)
        sorted.resize(k);
    return sorted;
}

}  // namespace dht
