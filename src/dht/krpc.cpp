#include "dht/krpc.h"

#include <cstring>
#include <iomanip>
#include <map>
#include <random>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#include "bencode/bencode_encoder.h"
#include "bencode/bencode_parser.h"
#include "bencode/bencode_types.h"
#include "peer_address.h"

namespace dht
{

// ---------------------------------------------------------------------------
// Message builders
// ---------------------------------------------------------------------------

std::string make_ping(const std::string& txn, const NodeId& self_id)
{
    std::string args =
        bencode::dict({{"id", bencode::string(self_id.to_string())}});
    return bencode::dict({{"a", args},
                          {"q", bencode::string("ping")},
                          {"t", bencode::string(txn)},
                          {"y", bencode::string("q")}});
}

std::string make_find_node(const std::string& txn,
                           const NodeId& self_id,
                           const NodeId& target)
{
    std::string args =
        bencode::dict({{"id", bencode::string(self_id.to_string())},
                       {"target", bencode::string(target.to_string())}});
    return bencode::dict({{"a", args},
                          {"q", bencode::string("find_node")},
                          {"t", bencode::string(txn)},
                          {"y", bencode::string("q")}});
}

std::string make_get_peers(const std::string& txn,
                           const NodeId& self_id,
                           const std::string& info_hash_20)
{
    std::string args =
        bencode::dict({{"id", bencode::string(self_id.to_string())},
                       {"info_hash", bencode::string(info_hash_20)}});
    return bencode::dict({{"a", args},
                          {"q", bencode::string("get_peers")},
                          {"t", bencode::string(txn)},
                          {"y", bencode::string("q")}});
}

std::string make_announce_peer(const std::string& txn,
                               const NodeId& self_id,
                               const std::string& info_hash_20,
                               uint16_t port,
                               const std::string& token,
                               bool implied_port)
{
    std::map<std::string, std::string> a_map;
    a_map["id"] = bencode::string(self_id.to_string());
    a_map["implied_port"] = bencode::integer(implied_port ? 1 : 0);
    a_map["info_hash"] = bencode::string(info_hash_20);
    a_map["port"] = bencode::integer(port);
    a_map["token"] = bencode::string(token);
    return bencode::dict({{"a", bencode::dict(a_map)},
                          {"q", bencode::string("announce_peer")},
                          {"t", bencode::string(txn)},
                          {"y", bencode::string("q")}});
}

std::string make_response(const std::string& txn, const NodeId& self_id)
{
    std::string r =
        bencode::dict({{"id", bencode::string(self_id.to_string())}});
    return bencode::dict(
        {{"r", r}, {"t", bencode::string(txn)}, {"y", bencode::string("r")}});
}

static std::string nodes_to_compact(const std::vector<RoutingEntry>& nodes)
{
    std::string out;
    out.reserve(nodes.size() * 26);
    for (const auto& n : nodes)
        out += n.compact();
    return out;
}

std::string make_nodes_response(const std::string& txn,
                                const NodeId& self_id,
                                const std::vector<RoutingEntry>& nodes)
{
    std::string r =
        bencode::dict({{"id", bencode::string(self_id.to_string())},
                       {"nodes", bencode::string(nodes_to_compact(nodes))}});
    return bencode::dict(
        {{"r", r}, {"t", bencode::string(txn)}, {"y", bencode::string("r")}});
}

std::string make_peers_response(const std::string& txn,
                                const NodeId& self_id,
                                const std::string& token,
                                const std::vector<PeerAddress>& pas)
{
    std::vector<std::string> encoded;
    encoded.reserve(pas.size());
    for (const auto& p : pas)
        encoded.push_back(bencode::string(p.to_string()));

    std::string r = bencode::dict({{"id", bencode::string(self_id.to_string())},
                                   {"token", bencode::string(token)},
                                   {"values", bencode::list(encoded)}});
    return bencode::dict(
        {{"r", r}, {"t", bencode::string(txn)}, {"y", bencode::string("r")}});
}

std::string make_nodes_response_gp(const std::string& txn,
                                   const NodeId& self_id,
                                   const std::string& token,
                                   const std::vector<RoutingEntry>& nodes)
{
    std::string r =
        bencode::dict({{"id", bencode::string(self_id.to_string())},
                       {"nodes", bencode::string(nodes_to_compact(nodes))},
                       {"token", bencode::string(token)}});
    return bencode::dict(
        {{"r", r}, {"t", bencode::string(txn)}, {"y", bencode::string("r")}});
}

std::string make_error(const std::string& txn, int code, const std::string& msg)
{
    std::vector<std::string> err_list = {bencode::integer(code),
                                         bencode::string(msg)};
    return bencode::dict({{"e", bencode::list(err_list)},
                          {"t", bencode::string(txn)},
                          {"y", bencode::string("e")}});
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string random_txn()
{
    static std::mt19937 gen(std::random_device{}());
    static std::uniform_int_distribution<unsigned> dist(0, 255);
    std::string t(2, '\0');
    t[0] = static_cast<char>(dist(gen));
    t[1] = static_cast<char>(dist(gen));
    return t;
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

// Helper: safely extract a BString value from a BDict.
static std::string bstring(BDict* d, const std::string& key)
{
    auto it = d->content.find(key);
    if (it == d->content.end())
        return {};
    auto* bs = dynamic_cast<BString*>(it->second.get());
    if (!bs)
        return {};
    return bs->content;
}

static int64_t binteger(BDict* d, const std::string& key, int64_t def = 0)
{
    auto it = d->content.find(key);
    if (it == d->content.end())
        return def;
    auto* bi = dynamic_cast<BInteger*>(it->second.get());
    if (!bi)
        return def;
    return bi->value;
}

static BDict* bdict(BDict* d, const std::string& key)
{
    auto it = d->content.find(key);
    if (it == d->content.end())
        return nullptr;
    return dynamic_cast<BDict*>(it->second.get());
}

static BList* blist(BDict* d, const std::string& key)
{
    auto it = d->content.find(key);
    if (it == d->content.end())
        return nullptr;
    return dynamic_cast<BList*>(it->second.get());
}

static void parse_nodes(const std::string& compact,
                        std::vector<RoutingEntry>& nodes,
                        std::vector<size_t>& counts)
{
    nodes.clear();
    counts.clear();
    std::unordered_map<std::string, size_t> index;
    index.reserve(compact.size() / 26);

    for (size_t i = 0; i + 26 <= compact.size(); i += 26)
    {
        const std::string key = compact.substr(i, 26);
        const auto it = index.find(key);
        if (it != index.end())
        {
            counts[it->second]++;
            continue;
        }
        index.emplace(key, nodes.size());
        nodes.push_back(RoutingEntry::from_compact(compact, i));
        counts.push_back(1);
    }
}

static std::vector<std::string> parse_peers(BList* lst)
{
    std::vector<std::string> out;
    if (!lst)
        return out;
    for (const auto& item : lst->content)
    {
        auto* bs = dynamic_cast<BString*>(item.get());
        if (bs && bs->content.size() == 6)
            out.push_back(bs->content);
    }
    return out;
}

std::optional<KrpcMessage> parse_krpc(const std::string& data)
{
    try
    {
        bencode::Parser parser(data, true);
        auto root_ptr = parser.parse_value();
        if (!root_ptr)
            return std::nullopt;

        auto* root = dynamic_cast<BDict*>(root_ptr.get());
        if (!root)
            return std::nullopt;

        KrpcMessage msg;
        msg.txn = bstring(root, "t");

        std::string y = bstring(root, "y");
        if (y == "q")
        {
            msg.type = KrpcType::Query;
            std::string q = bstring(root, "q");
            BDict* a = bdict(root, "a");
            if (!a)
                return std::nullopt;

            msg.sender_id = NodeId::from_string(bstring(a, "id"));

            if (q == "ping")
            {
                msg.query_type = KrpcQuery::Ping;
            }
            else if (q == "find_node")
            {
                msg.query_type = KrpcQuery::FindNode;
                msg.target = NodeId::from_string(bstring(a, "target"));
            }
            else if (q == "get_peers")
            {
                msg.query_type = KrpcQuery::GetPeers;
                msg.info_hash = bstring(a, "info_hash");
            }
            else if (q == "announce_peer")
            {
                msg.query_type = KrpcQuery::AnnouncePeer;
                msg.info_hash = bstring(a, "info_hash");
                msg.token = bstring(a, "token");
                msg.peer_port = static_cast<uint16_t>(binteger(a, "port"));
                msg.implied_port = (binteger(a, "implied_port") != 0);
            }
            else
            {
                return std::nullopt;
            }
        }
        else if (y == "r")
        {
            msg.type = KrpcType::Response;
            BDict* r = bdict(root, "r");
            if (!r)
                return std::nullopt;

            msg.sender_id = NodeId::from_string(bstring(r, "id"));

            std::string nodes_raw = bstring(r, "nodes");
            if (!nodes_raw.empty())
                parse_nodes(nodes_raw, msg.nodes, msg.node_counts);

            msg.token = bstring(r, "token");
            msg.peers = parse_peers(blist(r, "values"));
        }
        else if (y == "e")
        {
            msg.type = KrpcType::Error;
            BList* e = blist(root, "e");
            if (e && e->content.size() >= 2)
            {
                auto* code_node = dynamic_cast<BInteger*>(e->content[0].get());
                auto* msg_node = dynamic_cast<BString*>(e->content[1].get());
                if (code_node)
                    msg.error_code = static_cast<int>(code_node->value);
                if (msg_node)
                    msg.error_msg = msg_node->content;
            }
        }
        else
        {
            return std::nullopt;
        }

        return msg;
    }
    catch (...)
    {
        return std::nullopt;
    }
}

namespace
{

std::string txn_hex(const std::string& txn)
{
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (unsigned char c : txn)
        oss << std::setw(2) << static_cast<unsigned>(c);
    return oss.str();
}

std::string bytes_hex_prefix(const std::string& bytes, size_t n = 8)
{
    if (bytes.empty())
        return "";
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < std::min(n, bytes.size()); ++i)
    {
        const unsigned char byte = static_cast<unsigned char>(bytes[i]);
        oss << std::setw(2) << static_cast<unsigned>(byte);
    }
    if (bytes.size() > n)
        oss << "...";
    return oss.str();
}

std::string bytes_hex(const std::string& bytes)
{
    if (bytes.empty())
        return "";
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (unsigned char byte : bytes)
        oss << std::setw(2) << static_cast<unsigned>(byte);
    return oss.str();
}

std::string format_node_id_prefix(const NodeId& id)
{
    const std::string hex = id.hex();
    if (hex.size() <= 8)
        return hex;
    return hex.substr(0, 8) + "...";
}

std::string format_nodes(const std::vector<RoutingEntry>& nodes,
                         const std::vector<size_t>& counts)
{
    std::ostringstream oss;
    oss << '[';
    for (size_t i = 0; i < nodes.size(); ++i)
    {
        if (i > 0)
            oss << ", ";
        oss << "id=" << format_node_id_prefix(nodes[i].id) << " @ "
            << nodes[i].ip << ':' << nodes[i].port;
        const size_t repeat = (i < counts.size()) ? counts[i] : 1;
        if (repeat > 1)
            oss << " x" << repeat;
    }
    oss << ']';
    return oss.str();
}

std::pair<size_t, std::string> format_peers(
    const std::vector<std::string>& compact_peers)
{
    std::ostringstream oss;
    oss << '[';
    size_t count = 0;
    for (size_t i = 0; i < compact_peers.size(); ++i)
    {
        std::string ip;
        uint16_t port = 0;
        if (!compact_to_peer(compact_peers[i], 0, ip, port))
            continue;
        if (count > 0)
            oss << ", ";
        ++count;
        oss << ip << ':' << port;
    }
    oss << ']';
    return {count, oss.str()};
}

}  // namespace

std::string format_krpc_summary(const KrpcMessage& msg)
{
    std::ostringstream oss;
    oss << "transaction_id=" << txn_hex(msg.txn);

    switch (msg.type)
    {
        case KrpcType::Query:
            oss << " QUERY";
            if (!msg.query_type)
            {
                oss << " q=?";
                break;
            }
            switch (*msg.query_type)
            {
                case KrpcQuery::Ping:
                    oss << " PING";
                    break;
                case KrpcQuery::FindNode:
                    oss << " FIND_NODE target=" << msg.target.hex().substr(0, 8)
                        << "...";
                    break;
                case KrpcQuery::GetPeers:
                    oss << " GET_PEERS info_hash="
                        << bytes_hex_prefix(msg.info_hash);
                    break;
                case KrpcQuery::AnnouncePeer:
                    oss << " ANNOUNCE_PEERS info_hash="
                        << bytes_hex_prefix(msg.info_hash)
                        << " port=" << msg.peer_port
                        << (msg.implied_port ? " implied_port=1" : "");
                    break;
            }
            break;

        case KrpcType::Response:
            oss << " RESPONSE";
            if (!msg.nodes.empty())
            {
                size_t wire_count = 0;
                if (!msg.node_counts.empty())
                {
                    for (size_t c : msg.node_counts)
                        wire_count += c;
                }
                else
                {
                    wire_count = msg.nodes.size();
                }
                oss << "\n    nodes(" << wire_count
                    << ")=" << format_nodes(msg.nodes, msg.node_counts);
            }
            if (!msg.peers.empty())
            {
                const auto [peer_count, peer_list] = format_peers(msg.peers);
                oss << "\n    peers(" << peer_count << ")=" << peer_list;
            }
            if (!msg.token.empty())
                oss << "\n    token=" << bytes_hex(msg.token);
            if (!msg.sender_id.is_zero())
                oss << "\n    id=" << msg.sender_id.hex().substr(0, 8) << "...";
            break;

        case KrpcType::Error:
            oss << " ERROR code=" << msg.error_code << " msg=\""
                << msg.error_msg << '"';
            break;
    }

    if (!msg.sender_id.is_zero() && msg.type != KrpcType::Response)
        oss << " id=" << msg.sender_id.hex().substr(0, 8) << "...";

    return oss.str();
}

}  // namespace dht
