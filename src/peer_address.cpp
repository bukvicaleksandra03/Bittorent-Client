#include "peer_address.h"

#include <arpa/inet.h>
#include <cstring>

std::string peer_to_compact(const std::string& ip, uint16_t port)
{
    std::string out(6, '\0');
    struct in_addr addr{};
    // Convert dotted-decimal IPv4 (e.g. "192.168.1.1") to 4-byte network order.
    inet_aton(ip.c_str(), &addr);
    std::memcpy(out.data(), &addr.s_addr, 4);
    // Host port to network byte order (big-endian) for the wire format.
    uint16_t p = htons(port);
    std::memcpy(out.data() + 4, &p, 2);
    return out;
}

bool compact_to_peer(const std::string& data, size_t offset,
                     std::string& out_ip, uint16_t& out_port)
{
    if (data.size() < offset + 6)
        return false;
    uint32_t ip_raw{};
    std::memcpy(&ip_raw, data.data() + offset, 4);
    struct in_addr addr{};
    addr.s_addr = ip_raw;
    // Convert 4-byte network-order IPv4 back to dotted-decimal string.
    out_ip = inet_ntoa(addr);
    uint16_t p{};
    std::memcpy(&p, data.data() + offset + 4, 2);
    // Network byte order port back to host byte order for local use.
    out_port = ntohs(p);
    return true;
}
