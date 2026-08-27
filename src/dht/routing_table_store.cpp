#include "dht/routing_table_store.h"

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unistd.h>

#include <sys/stat.h>

namespace dht
{

namespace
{

bool ensure_parent_dir(const std::string& path, std::string* error_out)
{
    const auto pos = path.find_last_of('/');
    if (pos == std::string::npos)
        return true;

    const std::string dir = path.substr(0, pos);
    if (dir.empty())
        return true;

    struct stat st
    {
    };
    if (stat(dir.c_str(), &st) == 0)
        return true;

    if (mkdir(dir.c_str(), 0755) != 0)
    {
        if (error_out)
            *error_out = "failed to create directory: " + dir;
        return false;
    }
    return true;
}

bool node_id_from_hex(const std::string& hex, NodeId* out)
{
    if (hex.size() != 40 || !out)
        return false;

    NodeId id;
    for (size_t i = 0; i < 20; ++i)
    {
        const unsigned byte =
            std::stoul(hex.substr(i * 2, 2), nullptr, 16);
        id.bytes[i] = static_cast<uint8_t>(byte);
    }
    *out = id;
    return true;
}

bool parse_entry_line(const std::string& line, RoutingEntry* out)
{
    if (line.size() < 6 || line.compare(0, 6, "entry=") != 0)
        return false;

    std::istringstream iss(line.substr(6));
    std::string id_hex;
    std::string ip;
    uint16_t port = 0;
    if (!(iss >> id_hex >> ip >> port))
        return false;
    NodeId id;
    if (!node_id_from_hex(id_hex, &id))
        return false;

    RoutingEntry entry;
    entry.id = id;
    entry.pa = PeerAddress(ip, port);
    // Not "good" until we hear from the node again (see DhtClient startup verify).
    entry.last_seen = std::chrono::steady_clock::time_point{};
    *out = entry;
    return true;
}

}  // namespace

std::string RoutingTableStore::default_path()
{
    if (const char* env = std::getenv("DHT_ROUTING_TABLE_PATH"))
        return env;
    return "routing_table_store/routing_table.txt";
}

RoutingTableStoreResult RoutingTableStore::load(const std::string& path)
{
    RoutingTableStoreResult result;
    std::ifstream in(path);
    if (!in)
    {
        result.error = "file not found: " + path;
        return result;
    }

    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty() || line[0] == '#')
            continue;

        if (line.rfind("version=", 0) == 0)
        {
            if (line != "version=1")
            {
                result.error = "unsupported routing table version: " + line;
                return result;
            }
            continue;
        }

        if (line.rfind("self_id=", 0) == 0)
        {
            NodeId id;
            if (node_id_from_hex(line.substr(8), &id))
                result.snapshot.self_id = id;
            continue;
        }

        if (line.rfind("entry=", 0) == 0)
        {
            RoutingEntry entry;
            if (!parse_entry_line(line, &entry))
            {
                result.error = "malformed entry line: " + line;
                return result;
            }
            result.snapshot.entries.push_back(entry);
            continue;
        }
    }

    if (result.snapshot.entries.empty())
    {
        result.error = "no entries in routing table file";
        return result;
    }

    result.ok = true;
    return result;
}

bool RoutingTableStore::save(const RoutingTable& table,
                             const NodeId& self_id,
                             const std::string& path,
                             std::string* error_out)
{
    if (!ensure_parent_dir(path, error_out))
        return false;

    const std::vector<RoutingEntry> entries = table.all();
    if (entries.empty())
        return true;

    std::ofstream out(path, std::ios::trunc);
    if (!out)
    {
        if (error_out)
            *error_out = "failed to open for write: " + path;
        return false;
    }

    out << "# DHT routing table (auto-generated)\n";
    out << "version=1\n";
    out << "self_id=" << self_id.hex() << "\n";
    for (const auto& e : entries)
        out << "entry=" << e.id.hex() << " " << e.pa.ip << " "
            << e.pa.port << "\n";

    return out.good();
}

bool RoutingTableStore::remove_file(const std::string& path,
                                      std::string* error_out)
{
    if (path.empty())
        return true;
    if (remove(path.c_str()) != 0 && errno != ENOENT)
    {
        if (error_out)
            *error_out = "failed to remove: " + path;
        return false;
    }
    return true;
}

}  // namespace dht
