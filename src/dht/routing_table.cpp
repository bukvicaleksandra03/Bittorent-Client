#include "dht/routing_table.h"

#include <arpa/inet.h>

#include <algorithm>
#include <cstring>
#include <random>
#include <string>

#include "peer_address.h"

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
    std::memcpy(out.data(), id.bytes.data(), 20);
    struct in_addr addr
    {
    };
    inet_aton(pa.ip.c_str(), &addr);
    std::memcpy(out.data() + 20, &addr.s_addr, 4);
    uint16_t port_be = htons(pa.port);
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
    uint16_t port_be{};
    std::memcpy(&port_be, data.data() + offset + 24, 2);
    entry.pa = PeerAddress(inet_ntoa(addr), ntohs(port_be));

    entry.last_seen = std::chrono::steady_clock::now();
    return entry;
}

// ---------------------------------------------------------------------------
// RoutingTable
// ---------------------------------------------------------------------------

RoutingTable::RoutingTable(const NodeId& self_id) : self_id_(self_id)
{
    bucket_sizes_.fill(0);
    peers_received_per_bucket_.fill(0);
}

void RoutingTable::set_dht_logger(std::shared_ptr<logger::Logger> logger)
{
    std::lock_guard<std::mutex> lock(mutex_);
    dht_logger_ = std::move(logger);
}

void RoutingTable::log_error(const std::string& message) const
{
    if (dht_logger_)
        dht_logger_->error("[Routing Table] " + message);
}

void RoutingTable::log_info(const std::string& message) const
{
    if (dht_logger_)
        dht_logger_->info("[Routing Table] " + message);
}

void RoutingTable::log_debug(const std::string& message) const
{
    if (dht_logger_)
        dht_logger_->debug("[Routing Table] " + message);
}

void RoutingTable::log_bucket_content(size_t bucket,
                                      const std::string& label) const
{
    if (bucket >= NUM_BUCKETS)
        return;

    log_debug("Bucket " + std::to_string(bucket) + " " + label + " (" +
              std::to_string(bucket_sizes_[bucket]) + " nodes):");
    for (size_t i = 0; i < bucket_sizes_[bucket]; ++i)
    {
        log_debug("  [" + std::to_string(i) + "] " +
                  buckets_[bucket][i].to_string_detailed());
    }
}

size_t RoutingTable::bucket_index(const NodeId& id) const
{
    NodeId dist = self_id_ ^ id;
    if (dist.is_zero())
        return NUM_BUCKETS;

    for (size_t byte_idx = 0; byte_idx < dist.bytes.size(); ++byte_idx)
    {
        uint8_t b = dist.bytes[byte_idx];
        if (b == 0)
            continue;
        for (int bit = 7; bit >= 0; --bit)
        {
            if (b & (1u << bit))
                return byte_idx * 8 + static_cast<size_t>(7 - bit);
        }
    }
    return NUM_BUCKETS - 1;
}

RoutingEntry* RoutingTable::find_in_bucket(size_t bucket, const NodeId& id)
{
    if (bucket >= NUM_BUCKETS)
        return nullptr;
    for (size_t i = 0; i < bucket_sizes_[bucket]; ++i)
    {
        if (buckets_[bucket][i].id == id)
            return &buckets_[bucket][i];
    }
    return nullptr;
}

void RoutingTable::touch_bucket_entry(size_t bucket,
                                      size_t idx,
                                      const RoutingEntry& entry)
{
    if (idx >= bucket_sizes_[bucket])
        return;
    buckets_[bucket][idx] = entry;
    // Move to end (most recently seen).
    if (idx + 1 < bucket_sizes_[bucket])
    {
        RoutingEntry updated = buckets_[bucket][idx];
        for (size_t i = idx; i + 1 < bucket_sizes_[bucket]; ++i)
            buckets_[bucket][i] = buckets_[bucket][i + 1];
        buckets_[bucket][bucket_sizes_[bucket] - 1] = updated;
    }
}

void RoutingTable::add(const RoutingEntry& entry, bool count_as_received)
{
    if (entry.id == self_id_ || entry.id.is_zero())
        return;

    std::lock_guard<std::mutex> lock(mutex_);

    const size_t bucket = bucket_index(entry.id);
    if (bucket >= NUM_BUCKETS)
    {
        log_error("routing add: no bucket for node " + entry.to_string());
        return;
    }

    if (count_as_received)
        ++peers_received_per_bucket_[bucket];

    log_info("Adding routing entry " + entry.to_string() + " to bucket " +
             std::to_string(bucket));
    log_bucket_content(bucket, "before");

    if (RoutingEntry* existing = find_in_bucket(bucket, entry.id))
    {
        *existing = entry;
        const size_t idx =
            static_cast<size_t>(existing - buckets_[bucket].data());
        touch_bucket_entry(bucket, idx, entry);
        log_info("The entry " + entry.to_string() +
                 " already exists in the table. Moved to be MRU.");
        log_bucket_content(bucket, "after");
        return;
    }

    if (bucket_sizes_[bucket] < K)
    {
        buckets_[bucket][bucket_sizes_[bucket]++] = entry;
        log_info("The entry " + entry.to_string() + " added to bucket " +
                 std::to_string(bucket) + ". New bucket size is " +
                 std::to_string(bucket_sizes_[bucket]));
        log_bucket_content(bucket, "after");
        return;
    }

    // Bucket full: replace the least-recently seen non-good entry, else drop.
    size_t replace_idx = 0;
    auto oldest = buckets_[bucket][0].last_seen;
    for (size_t i = 1; i < bucket_sizes_[bucket]; ++i)
    {
        if (buckets_[bucket][i].last_seen < oldest)
        {
            oldest = buckets_[bucket][i].last_seen;
            replace_idx = i;
        }
    }

    if (!buckets_[bucket][replace_idx].is_good())
    {
        buckets_[bucket][replace_idx] = entry;
        log_info("The entry " + entry.to_string() +
                 " replaced stale node in bucket " + std::to_string(bucket));
    }
    else
    {
        log_info("The entry " + entry.to_string() + " dropped: bucket " +
                 std::to_string(bucket) + " is full");
    }
    log_bucket_content(bucket, "after");
}

void RoutingTable::remove(const NodeId& id)
{
    const size_t bucket = bucket_index(id);
    if (bucket >= NUM_BUCKETS)
        return;

    std::lock_guard<std::mutex> lock(mutex_);

    for (size_t i = 0; i < bucket_sizes_[bucket]; ++i)
    {
        if (buckets_[bucket][i].id != id)
            continue;
        for (size_t j = i + 1; j < bucket_sizes_[bucket]; ++j)
            buckets_[bucket][j - 1] = buckets_[bucket][j];
        --bucket_sizes_[bucket];
        return;
    }
}

size_t RoutingTable::size() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    size_t total = 0;
    for (size_t n : bucket_sizes_)
        total += n;
    return total;
}

RoutingTable::BucketSnapshot RoutingTable::bucket_occupancy() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return bucket_sizes_;
}

RoutingTable::BucketSnapshot RoutingTable::peers_received_per_bucket() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return peers_received_per_bucket_;
}

std::vector<RoutingEntry> RoutingTable::all_unlocked() const
{
    std::vector<RoutingEntry> out;
    for (size_t b = 0; b < NUM_BUCKETS; ++b)
    {
        for (size_t i = 0; i < bucket_sizes_[b]; ++i)
            out.push_back(buckets_[b][i]);
    }
    return out;
}

std::vector<RoutingEntry> RoutingTable::all() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return all_unlocked();
}

std::vector<RoutingEntry> RoutingTable::closest(const NodeId& target,
                                                size_t k) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RoutingEntry> sorted = all_unlocked();
    std::sort(sorted.begin(),
              sorted.end(),
              [&target](const RoutingEntry& a, const RoutingEntry& b)
              { return (target ^ a.id) < (target ^ b.id); });

    if (sorted.size() > k)
        sorted.resize(k);
    return sorted;
}

// A stale bucket is refreshed by looking up a random ID inside it: the nodes
// closest to that target are exactly the ones that belong in the bucket, so the
// lookup yields fresh candidates for the quiet region of the keyspace. The
// target is randomized so repeated refreshes probe different points in the
// bucket instead of converging on the same few nodes.
NodeId RoutingTable::random_id_in_bucket(size_t bucket) const
{
    NodeId id = self_id_;
    if (bucket >= NUM_BUCKETS)
        bucket = NUM_BUCKETS - 1;

    static thread_local std::mt19937 gen(std::random_device{}());
    static thread_local std::uniform_int_distribution<int> bit_dist(0, 1);

    const size_t byte_idx = bucket / 8;
    const int bit_in_byte = 7 - static_cast<int>(bucket % 8);

    id.bytes[byte_idx] ^=
        static_cast<uint8_t>(1u << static_cast<unsigned>(bit_in_byte));

    for (size_t b = bucket + 1; b < NUM_BUCKETS; ++b)
    {
        const size_t bi = b / 8;
        const int bb = 7 - static_cast<int>(b % 8);
        if (bit_dist(gen))
            id.bytes[bi] |=
                static_cast<uint8_t>(1u << static_cast<unsigned>(bb));
        else
            id.bytes[bi] &= static_cast<uint8_t>(
                ~static_cast<uint8_t>(1u << static_cast<unsigned>(bb)));
    }

    return id;
}

std::vector<size_t> RoutingTable::buckets_needing_refresh(
    std::chrono::steady_clock::time_point now,
    std::chrono::minutes max_age) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<size_t> out;
    for (size_t b = 0; b < NUM_BUCKETS; ++b)
    {
        if (bucket_sizes_[b] == 0)
        {
            out.push_back(b);
            continue;
        }
        bool stale = true;
        for (size_t i = 0; i < bucket_sizes_[b]; ++i)
        {
            if (now - buckets_[b][i].last_seen < max_age)
            {
                stale = false;
                break;
            }
        }
        if (stale)
            out.push_back(b);
    }
    return out;
}

}  // namespace dht
