#pragma once

#include <cstdint>
#include <optional>
#include <string>

// RAII wrapper around a UPnP IGD (Internet Gateway Device) TCP port mapping.
//
// Usage:
//   auto mapping = UPnPPortMapping::create(6881);
//   if (mapping)
//       std::cout << "external IP: " << mapping->external_ip() << "\n";
//   // destructor removes the mapping automatically
//
// The class discovers the gateway via UPnP SSDP, requests a TCP port mapping
// from the WAN side, and removes it on destruction.  It is move-only and
// non-copyable.
class UPnPPortMapping
{
   public:
    // Discovers a UPnP gateway and maps external_port -> internal_port (TCP).
    // If external_port is 0, internal_port is used for both sides.
    // Returns std::nullopt if:
    //   - No UPnP gateway is found within the discovery timeout.
    //   - The gateway rejects the mapping request.
    //   - Any network error occurs.
    static std::optional<UPnPPortMapping> create(uint16_t internal_port,
                                                 uint16_t external_port = 0);

    ~UPnPPortMapping();

    UPnPPortMapping(const UPnPPortMapping&) = delete;
    UPnPPortMapping& operator=(const UPnPPortMapping&) = delete;

    UPnPPortMapping(UPnPPortMapping&&) noexcept;
    UPnPPortMapping& operator=(UPnPPortMapping&&) noexcept;

    // The port opened on the router's WAN interface.
    uint16_t external_port() const { return m_external_port; }

    // The WAN/public IP address of the gateway (the address remote peers
    // should connect to).
    const std::string& external_ip() const { return m_external_ip; }

   private:
    UPnPPortMapping() = default;

    void release() noexcept;

    std::string m_control_url;   // SOAP endpoint for the IGD service
    std::string m_service_type;  // e.g. "urn:schemas-upnp-org:service:WANIPConnection:1"
    uint16_t m_external_port{0};
    std::string m_external_ip;
    bool m_valid{false};  // false after move or failed create()
};
