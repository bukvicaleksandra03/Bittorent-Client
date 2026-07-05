/*
 * Manual UPnP IGD port-mapping implementation.
 *
 * Protocol sequence
 * -----------------
 * 1. Send a UDP SSDP M-SEARCH multicast to 239.255.255.250:1900.
 * 2. The router replies with an HTTP-like response containing a LOCATION URL
 *    that points to its XML device description.
 * 3. HTTP-GET that URL to retrieve the XML.
 * 4. Parse the XML to find the WANIPConnection (or WANPPPConnection)
 *    <controlURL> and <serviceType>.
 * 5. Send a SOAP AddPortMapping request to the control URL.
 * 6. On destruction, send a SOAP DeletePortMapping request.
 *
 */

#include "net/upnp.h"

#include <arpa/inet.h>  // inet_ntop, in_addr  (default_gateway_ip)

#include <algorithm>
#include <chrono>
#include <cstdio>  // fopen/fgets/fclose/sscanf  (default_gateway_ip)
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "net/socket.h"
#include "net/socket_addresses.h"

// ─── tiny helpers ────────────────────────────────────────────────────────────

static std::string to_str(uint16_t n)
{
    return std::to_string(static_cast<unsigned>(n));
}

// Return the first substring between <tag> and </tag>, or "" if not found.
static std::string xml_tag(const std::string& xml, const std::string& tag)
{
    std::string open = "<" + tag + ">";
    std::string close = "</" + tag + ">";
    auto a = xml.find(open);
    if (a == std::string::npos)
        return {};
    a += open.size();
    auto b = xml.find(close, a);
    if (b == std::string::npos)
        return {};
    return xml.substr(a, b - a);
}

// Case-insensitive header value from a raw HTTP response.
// Looks for "Header-Name: value\r\n".
static std::string http_header(const std::string& resp, const std::string& name)
{
    // try both capitalizations
    for (auto& needle : {name + ": ", name + ":"})
    {
        auto lo = resp;
        auto ln = needle;
        for (auto& c : lo)
            c = (char)tolower((unsigned char)c);
        for (auto& c : ln)
            c = (char)tolower((unsigned char)c);
        auto pos = lo.find(ln);
        if (pos == std::string::npos)
            continue;
        pos += needle.size();
        auto end = resp.find('\n', pos);
        auto val = resp.substr(
            pos, end == std::string::npos ? std::string::npos : end - pos);
        while (!val.empty() && (val.back() == '\r' || val.back() == ' '))
            val.pop_back();
        return val;
    }
    return {};
}

// ─── URL parsing ─────────────────────────────────────────────────────────────

struct Url
{
    std::string host;
    uint16_t port{80};
    std::string path;
};

// Parse "http://host[:port]/path".  Returns nullopt on failure.
static std::optional<Url> parse_url(const std::string& raw)
{
    // strip scheme
    std::string s = raw;
    auto scheme_end = s.find("://");
    if (scheme_end != std::string::npos)
        s = s.substr(scheme_end + 3);

    Url u;
    auto slash = s.find('/');
    std::string host_part =
        (slash == std::string::npos) ? s : s.substr(0, slash);
    u.path = (slash == std::string::npos) ? "/" : s.substr(slash);

    auto colon = host_part.rfind(':');
    if (colon != std::string::npos)
    {
        u.host = host_part.substr(0, colon);
        try
        {
            u.port = (uint16_t)std::stoul(host_part.substr(colon + 1));
        }
        catch (...)
        {
            return {};
        }
    }
    else
    {
        u.host = host_part;
        u.port = 80;
    }
    if (u.host.empty())
        return {};
    return u;
}

// Read the default-gateway IPv4 from /proc/net/route (Linux).
//
// The kernel exposes its routing table as a tab-separated text file.
// Each numeric field is a 32-bit hex value in little-endian byte order.
// Example row (default route):
//
//   Iface      Destination  Gateway   Flags  ...  Mask
//   wlp0s20f3  00000000     0164A8C0  0003   ...  00000000
//
// Destination 0x00000000 means "catch-all" (the default route).
// Gateway     0x0164A8C0 — reading the bytes right-to-left gives
//             C0.A8.64.01 = 192.168.100.1 (the router's LAN address).
//
// Returns "" on failure.
static std::string default_gateway_ip()
{
    FILE* f = std::fopen("/proc/net/route", "r");
    if (!f)
        return {};

    char line[256];
    if (!std::fgets(line, sizeof(line), f))  // skip header row
    {
        std::fclose(f);
        return {};
    }

    while (std::fgets(line, sizeof(line), f))
    {
        char iface[32]{};
        unsigned long dest = 0, gw = 0, flags = 0;
        if (std::sscanf(line, "%31s %lx %lx %lx", iface, &dest, &gw, &flags) <
            4)
            continue;

        if (dest == 0 &&
            gw != 0)  // default route: destination 0.0.0.0, gateway set
        {
            std::fclose(f);
            in_addr a{};
            a.s_addr =
                static_cast<uint32_t>(gw);  // already in network byte order
            char buf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &a, buf, sizeof(buf));  // → "192.168.100.1"
            return buf;
        }
    }

    std::fclose(f);
    return {};
}

// ─── TCP HTTP helper ─────────────────────────────────────────────────────────
// (defined here so ssdp_discover can call it in Phase 3)

// Forward declaration – defined further down.
static std::string local_ip_for(const std::string& gateway_host);

// Open a TCP connection to host:port and return the full raw HTTP response
// (headers + body) for the given request string.
static std::string tcp_request(const std::string& host,
                               uint16_t port,
                               const std::string& request)
{
    try
    {
        IPv4Address dest(host, port);
        TCPClientSocket sock(AF_INET);
        // connect_with_timeout handles the non-blocking connect + poll loop.
        sock.connect_with_timeout(dest, 5000);

        // Send the full request.
        sock.send(request.data(), request.size());

        // Read the response until the server closes the connection (HTTP/1.0
        // style: no Content-Length needed, peer closes after body).
        return sock.recv_all_with_timeout(5000);
    }
    catch (const std::exception&)
    {
        return {};
    }
}

// ─── SSDP discovery ──────────────────────────────────────────────────────────

// Send SSDP M-SEARCH (multicast then unicast fallback) and return the LOCATION
// header from the first IGD response.  Returns "" on timeout / failure.
//
// Two common failure modes are fixed here:
//   1. Without IP_MULTICAST_IF the OS may pick a Docker/VPN/loopback interface
//      rather than the LAN interface that actually reaches the router.
//   2. PPPoE routers advertise WANPPPConnection, not WANIPConnection.
//   3. Some ZTE routers don't answer multicast M-SEARCH but do answer unicast.
static std::string ssdp_discover(int total_ms)
{
    // ── Detect the correct outgoing LAN interface ─────────────────────────
    // Connect a throwaway UDP socket to the multicast address; the kernel
    // fills in the source address without sending any packet, telling us
    // which interface the router is reachable on.
    const std::string local_ip = local_ip_for("239.255.255.250");
    const std::string gw_ip    = default_gateway_ip();

    std::cout << "[UPnP] local interface: " << (local_ip.empty() ? "(none)" : local_ip)
              << ", gateway: " << (gw_ip.empty() ? "(none)" : gw_ip)
              << "\n";

    UDPServerSocket sock(AF_INET);
    // TTL 4: conventional for SSDP; 1 would suffice on a LAN but 4 is safer.
    sock.set_multicast_ttl(4);
    if (!local_ip.empty())
        sock.set_multicast_if(local_ip);

    // Fire one M-SEARCH, wait up to wait_ms ms, return first LOCATION seen.
    // host_hdr is what goes into the HOST field (multicast addr for mc, dest
    // IP for unicast).
    auto try_target = [&](const IPv4Address& dest,
                          const char* st,
                          int wait_ms,
                          const std::string& host_hdr) -> std::string
    {
        std::cout << "[UPnP]  -> " << dest.ip() << ":1900  ST=" << st << "  ("
                  << wait_ms << " ms)\n";

        const std::string req = std::string("M-SEARCH * HTTP/1.1\r\n") +
                                "HOST: " + host_hdr +
                                "\r\n"
                                "MAN: \"ssdp:discover\"\r\n"
                                "MX: 2\r\n"
                                "ST: " +
                                st + "\r\n\r\n";
        sock.sendto(req, dest);

        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(wait_ms);
        while (true)
        {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                return {};
            const int remaining_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                                      now)
                    .count());

            std::string resp;
            try
            {
                auto [data, sender] = sock.recvfrom_with_timeout(remaining_ms);
                resp = std::move(data);
            }
            catch (const std::exception&)
            {
                return {};  // timeout or error
            }
            if (resp.empty())
                return {};
            // Log first status line of whatever we received.
            auto nl = resp.find('\n');
            std::cout << "[UPnP]     got: "
                      << resp.substr(0,
                                     nl == std::string::npos ? resp.size() : nl)
                      << "\n";
            if (resp.find("200 OK") == std::string::npos &&
                resp.find("200 ok") == std::string::npos)
                continue;
            const std::string loc = http_header(resp, "LOCATION");
            if (!loc.empty())
                return loc;
        }
    };

    // Targets in order of specificity.  WANPPPConnection:1 is critical for
    // PPPoE routers; without it they often won't answer targeted queries.
    static const char* targets[] = {
        "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
        "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
        "urn:schemas-upnp-org:service:WANIPConnection:1",
        "urn:schemas-upnp-org:service:WANPPPConnection:1",
        "ssdp:all",
        nullptr};
    constexpr int N = 5;  // must equal the number of non-null entries above

    // Budget: 60 % for multicast, 40 % for unicast; minimum 300 ms per shot.
    const int mc_ms = std::max(300, total_ms * 6 / (N * 10));
    const int uni_ms = std::max(300, total_ms * 4 / (N * 10));

    // ── Phase 1: Multicast to 239.255.255.250:1900 ───────────────────────
    IPv4Address mcast_dest("239.255.255.250", 1900);

    std::cout << "[UPnP] Phase 1: multicast\n";
    for (int i = 0; targets[i]; ++i)
    {
        std::string loc =
            try_target(mcast_dest, targets[i], mc_ms, "239.255.255.250:1900");
        if (!loc.empty())
            return loc;
    }

    // ── Phase 2: Unicast directly to the default gateway ─────────────────
    // Some routers (especially ZTE) don't reply to multicast M-SEARCH but
    // do respond when the packet is addressed to them directly.
    if (!gw_ip.empty())
    {
        std::cout << "[UPnP] Phase 2: unicast to " << gw_ip << "\n";
        IPv4Address uni_dest(gw_ip, 1900);
        const std::string uni_host = gw_ip + ":1900";
        for (int i = 0; targets[i]; ++i)
        {
            std::string loc =
                try_target(uni_dest, targets[i], uni_ms, uni_host);
            if (!loc.empty())
                return loc;
        }
    }

    // ── Phase 3: HTTP probe (SSDP-silent routers like ZTE F6600P) ─────────
    // Some routers have the UPnP HTTP service running but never respond to
    // SSDP M-SEARCH (confirmed with ZTE F6600P firmware).  Try common UPnP
    // HTTP ports and well-known description paths directly.
    if (!gw_ip.empty())
    {
        std::cout << "[UPnP] Phase 3: HTTP probe on gateway " << gw_ip << "\n";

        // Ports commonly used by consumer-router UPnP stacks.
        static const uint16_t probe_ports[] = {
            52869,  // ZTE, some Huawei
            49152,  // Windows/MiniUPnPd default
            49153,
            5555,  // some TP-Link / D-Link
            8200,  // MiniDLNA / some Netgear
            1900,  // occasionally used for HTTP too
            0};
        // Paths sorted by likelihood.
        static const char* probe_paths[] = {"/gatedesc.xml",  // ZTE
                                            "/description.xml",
                                            "/rootDesc.xml",
                                            "/upnp/IGD.xml",
                                            "/igd.xml",
                                            "/InternetGatewayDevice.xml",
                                            "/BasicLevel_desc.xml",
                                            nullptr};

        for (int pi = 0; probe_ports[pi]; ++pi)
        {
            const uint16_t port = probe_ports[pi];
            for (int pj = 0; probe_paths[pj]; ++pj)
            {
                const std::string path = probe_paths[pj];
                const std::string req = "GET " + path +
                                        " HTTP/1.0\r\n"
                                        "Host: " +
                                        gw_ip + ":" + std::to_string(port) +
                                        "\r\n"
                                        "Connection: close\r\n\r\n";

                // tcp_request handles the TCP connect/send/recv loop.
                const std::string resp = tcp_request(gw_ip, port, req);

                if (resp.find("200") != std::string::npos &&
                    (resp.find("upnp") != std::string::npos ||
                     resp.find("UPnP") != std::string::npos ||
                     resp.find("InternetGatewayDevice") != std::string::npos ||
                     resp.find("WANIPConnection") != std::string::npos ||
                     resp.find("WANPPPConnection") != std::string::npos))
                {
                    const std::string loc =
                        "http://" + gw_ip + ":" + std::to_string(port) + path;
                    std::cout << "[UPnP] Phase 3 found UPnP description at "
                              << loc << "\n";
                    return loc;
                }
            }
        }
    }

    return {};
}

// HTTP GET a URL, return the body (everything after the blank line).
static std::string http_get(const Url& url)
{
    std::string req = "GET " + url.path +
                      " HTTP/1.0\r\n"
                      "Host: " +
                      url.host + ":" + to_str(url.port) +
                      "\r\n"
                      "Connection: close\r\n"
                      "\r\n";

    std::string resp = tcp_request(url.host, url.port, req);
    auto sep = resp.find("\r\n\r\n");
    if (sep == std::string::npos)
        return {};
    return resp.substr(sep + 4);
}

// ─── IGD XML description parsing ─────────────────────────────────────────────

struct IgdInfo
{
    std::string
        control_url;  // absolute URL, e.g. http://192.168.1.1:49000/ctl/IPConn
    std::string
        service_type;  // e.g. urn:schemas-upnp-org:service:WANIPConnection:1
};

// Download and parse the IGD device description XML.
// We look for WANIPConnection or WANPPPConnection service entries.
static std::optional<IgdInfo> fetch_igd_info(const std::string& location)
{
    auto loc_url = parse_url(location);
    if (!loc_url)
        return {};

    std::string xml = http_get(*loc_url);
    if (xml.empty())
        return {};

    // The XML can have multiple <service> blocks.  We walk through all of
    // them looking for a WAN*Connection type.
    static const char* wanted_types[] = {
        "WANIPConnection", "WANPPPConnection", nullptr};

    size_t search_from = 0;
    while (true)
    {
        auto svc_start = xml.find("<service>", search_from);
        if (svc_start == std::string::npos)
            break;
        auto svc_end = xml.find("</service>", svc_start);
        if (svc_end == std::string::npos)
            break;
        search_from = svc_end + 10;

        std::string block = xml.substr(svc_start, svc_end - svc_start + 10);
        std::string stype = xml_tag(block, "serviceType");
        std::string curl = xml_tag(block, "controlURL");
        if (stype.empty() || curl.empty())
            continue;

        bool match = false;
        for (int i = 0; wanted_types[i]; ++i)
            if (stype.find(wanted_types[i]) != std::string::npos)
            {
                match = true;
                break;
            }
        if (!match)
            continue;

        // controlURL may be relative (e.g. "/ctl/IPConn") or absolute.
        std::string full_ctrl;
        if (curl.find("http://") == 0 || curl.find("http://") == 0)
        {
            full_ctrl = curl;
        }
        else
        {
            full_ctrl =
                "http://" + loc_url->host + ":" + to_str(loc_url->port) + curl;
        }
        return IgdInfo{full_ctrl, stype};
    }
    return {};
}

// ─── SOAP helper ─────────────────────────────────────────────────────────────

// Send a SOAP action to control_url.  Returns the HTTP response body.
static std::string soap_action(const std::string& control_url,
                               const std::string& service_type,
                               const std::string& action,
                               const std::string& soap_body)
{
    auto url = parse_url(control_url);
    if (!url)
        return {};

    std::string envelope =
        "<?xml version=\"1.0\"?>\r\n"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body>" +
        soap_body + "</s:Body></s:Envelope>";

    std::string req = "POST " + url->path +
                      " HTTP/1.0\r\n"
                      "Host: " +
                      url->host + ":" + to_str(url->port) +
                      "\r\n"
                      "Content-Type: text/xml; charset=\"utf-8\"\r\n"
                      "SOAPAction: \"" +
                      service_type + "#" + action +
                      "\"\r\n"
                      "Content-Length: " +
                      std::to_string(envelope.size()) +
                      "\r\n"
                      "Connection: close\r\n"
                      "\r\n" +
                      envelope;

    std::string resp = tcp_request(url->host, url->port, req);
    auto sep = resp.find("\r\n\r\n");
    if (sep == std::string::npos)
        return {};
    return resp.substr(sep + 4);
}

// AddPortMapping SOAP body.
static std::string soap_add_port(const std::string& ns,
                                 const std::string& ext_port,
                                 const std::string& int_port,
                                 const std::string& int_host,
                                 const std::string& lease_duration)
{
    return "<u:AddPortMapping xmlns:u=\"" + ns +
           "\">"
           "<NewRemoteHost></NewRemoteHost>"
           "<NewExternalPort>" +
           ext_port +
           "</NewExternalPort>"
           "<NewProtocol>TCP</NewProtocol>"
           "<NewInternalPort>" +
           int_port +
           "</NewInternalPort>"
           "<NewInternalClient>" +
           int_host +
           "</NewInternalClient>"
           "<NewEnabled>1</NewEnabled>"
           "<NewPortMappingDescription>BitTorrent</NewPortMappingDescription>"
           "<NewLeaseDuration>" +
           lease_duration +
           "</NewLeaseDuration>"
           "</u:AddPortMapping>";
}

// DeletePortMapping SOAP body.
static std::string soap_delete_port(const std::string& ns,
                                    const std::string& ext_port)
{
    return "<u:DeletePortMapping xmlns:u=\"" + ns +
           "\">"
           "<NewRemoteHost></NewRemoteHost>"
           "<NewExternalPort>" +
           ext_port +
           "</NewExternalPort>"
           "<NewProtocol>TCP</NewProtocol>"
           "</u:DeletePortMapping>";
}

// Get the local IP used to reach the router (by querying getsockname on a
// connected UDP socket).
static std::string local_ip_for(const std::string& gateway_host)
{
    try
    {
        // Connect a throwaway UDP socket to the gateway; the kernel fills in
        // the source address without sending any packet, telling us which
        // interface we route through.
        UDPClientSocket sock(AF_INET);
        IPv4Address dest(gateway_host, 1900);
        sock.connect(dest);
        auto local = sock.local_address();
        return static_cast<IPv4Address*>(local.get())->ip();
    }
    catch (const std::exception&)
    {
        return {};
    }
}

// Get the external IP via GetExternalIPAddress SOAP call.
static std::string get_external_ip(const IgdInfo& igd)
{
    std::string body = "<u:GetExternalIPAddress xmlns:u=\"" + igd.service_type +
                       "\">"
                       "</u:GetExternalIPAddress>";
    std::string resp = soap_action(
        igd.control_url, igd.service_type, "GetExternalIPAddress", body);
    return xml_tag(resp, "NewExternalIPAddress");
}

// ─── Public API ──────────────────────────────────────────────────────────────

std::optional<UPnPPortMapping> UPnPPortMapping::create(uint16_t internal_port,
                                                       uint16_t external_port)
{
    // 1. SSDP discover
    std::cout << "[UPnP] sending SSDP M-SEARCH (multicast + unicast gateway, 5 "
                 "s budget)...\n";
    std::string location = ssdp_discover(5000 /*ms*/);
    if (location.empty())
    {
        std::cout
            << "[UPnP] SSDP discovery timed out — no UPnP gateway responded\n";
        return {};
    }
    std::cout << "[UPnP] discovered IGD at: " << location << '\n';

    // 2. Fetch IGD description
    auto igd_opt = fetch_igd_info(location);
    if (!igd_opt)
    {
        std::cout << "[UPnP] failed to fetch device description from "
                  << location << '\n';
        return {};
    }
    const IgdInfo& igd = *igd_opt;
    std::cout << "[UPnP] control URL: " << igd.control_url << '\n';

    // 3. Determine our LAN address
    auto loc_url = parse_url(location);
    if (!loc_url)
        return {};
    std::string lan_ip = local_ip_for(loc_url->host);
    if (lan_ip.empty())
        return {};
    std::cout << "[UPnP] our LAN IP: " << lan_ip << '\n';

    // 4. Get external IP for informational purposes
    std::string ext_ip = get_external_ip(igd);
    if (!ext_ip.empty())
        std::cout << "[UPnP] external IP: " << ext_ip << '\n';

    // 5. AddPortMapping – try permanent (lease = 0) first, then 1-hour lease.
    uint16_t ext_port = (external_port != 0) ? external_port : internal_port;
    std::string ext_port_s = to_str(ext_port);
    std::string int_port_s = to_str(internal_port);

    std::cout << "[UPnP] requesting port mapping: " << ext_ip << ':' << ext_port
              << " → " << lan_ip << ':' << internal_port << '\n';

    for (auto& lease : {"0", "3600"})
    {
        std::string body = soap_add_port(
            igd.service_type, ext_port_s, int_port_s, lan_ip, lease);
        std::string resp = soap_action(
            igd.control_url, igd.service_type, "AddPortMapping", body);
        // A successful AddPortMapping returns an empty response body or an
        // envelope without a Fault element.
        if (resp.find("errorCode") == std::string::npos &&
            resp.find("UPnPError") == std::string::npos)
        {
            std::cout << "[UPnP] port mapping successful (lease=" << lease
                      << ")\n";
            UPnPPortMapping m;
            m.m_control_url = igd.control_url;
            m.m_service_type = igd.service_type;
            m.m_external_port = ext_port;
            m.m_external_ip = ext_ip;
            m.m_valid = true;
            return m;
        }
        std::cout << "[UPnP] AddPortMapping returned error (resp snippet: "
                  << resp.substr(0, 200) << ")\n";
        // If the external port is taken, give up rather than iterating.
        break;
    }
    return {};
}

UPnPPortMapping::~UPnPPortMapping()
{
    release();
}

UPnPPortMapping::UPnPPortMapping(UPnPPortMapping&& o) noexcept
    : m_control_url(std::move(o.m_control_url)),
      m_service_type(std::move(o.m_service_type)),
      m_external_port(o.m_external_port),
      m_external_ip(std::move(o.m_external_ip)),
      m_valid(o.m_valid)
{
    o.m_valid = false;
}

UPnPPortMapping& UPnPPortMapping::operator=(UPnPPortMapping&& o) noexcept
{
    if (this != &o)
    {
        release();
        m_control_url = std::move(o.m_control_url);
        m_service_type = std::move(o.m_service_type);
        m_external_port = o.m_external_port;
        m_external_ip = std::move(o.m_external_ip);
        m_valid = o.m_valid;
        o.m_valid = false;
    }
    return *this;
}

void UPnPPortMapping::release() noexcept
{
    if (!m_valid)
        return;
    m_valid = false;
    try
    {
        std::string body =
            soap_delete_port(m_service_type, to_str(m_external_port));
        soap_action(m_control_url, m_service_type, "DeletePortMapping", body);
    }
    catch (...)
    {
    }
}
