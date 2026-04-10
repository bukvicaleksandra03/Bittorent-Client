#pragma once

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <stdexcept>
#include <string>

#include "logger.h"

inline std::string socket_type_to_string(int socket_type)
{
    switch (socket_type)
    {
        case SOCK_STREAM:
            return "SOCK_STREAM";
        case SOCK_DGRAM:
            return "SOCK_DGRAM";
        default:
            return "UNKNOWN SOCKET TYPE (" + std::to_string(socket_type) + ")";
    }
}

class Address
{
   public:
    enum Domain
    {
        UNIX,
        IPv4,
        IPv6
    };

    static std::string domain_to_string(int domain)
    {
        switch (domain)
        {
            case Domain::UNIX:
                return "UNIX";
            case Domain::IPv4:
                return "IPv4";
            case Domain::IPv6:
                return "IPv6";
            default:
                LOG_AND_THROW("Unknown domain: " + std::to_string(domain));
        }
    }
    std::string identifier;
    virtual ~Address() = default;
    virtual const sockaddr* sockaddr_ptr() const = 0;
    virtual socklen_t size() const = 0;
    virtual int domain() const = 0;
};

class UnixAddress : public Address
{
   public:
    sockaddr_un addr{};
    socklen_t len = sizeof(addr);

    explicit UnixAddress(const std::string& path)
    {
        addr.sun_family = AF_UNIX;
        if (path.size() >= sizeof(addr.sun_path))
            LOG_AND_THROW("Unix path too long");
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
        identifier = path;
    }

    UnixAddress(const sockaddr* address)
    {
        addr = *reinterpret_cast<const sockaddr_un*>(address);
        identifier = addr.sun_path;
    }

    const sockaddr* sockaddr_ptr() const
    {
        return reinterpret_cast<const sockaddr*>(&addr);
    }
    socklen_t size() const
    {
        return len;
    }
    int domain() const
    {
        return AF_UNIX;
    }
};

class IPv4Address : public Address
{
   public:
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    char addrStr[INET_ADDRSTRLEN];

    IPv4Address(const std::string& ip, uint16_t port)
    {
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0)
            LOG_AND_THROW("Invalid IPv4 address");
        identifier = ip + ":" + std::to_string(port);
    }

    IPv4Address(in_addr_t ip, uint16_t port)
    {
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = ip;
        inet_ntop(AF_INET, &addr.sin_addr.s_addr, addrStr, INET_ADDRSTRLEN);
        identifier =
            std::string(addrStr) + ":" + std::to_string(ntohs(addr.sin_port));
    }

    IPv4Address(const sockaddr* address)
    {
        addr = *reinterpret_cast<const sockaddr_in*>(address);
        inet_ntop(AF_INET, &addr.sin_addr.s_addr, addrStr, INET_ADDRSTRLEN);
        identifier = "IPv4: " + std::string(addrStr) + ":" +
                     std::to_string(ntohs(addr.sin_port));
    }

    const sockaddr* sockaddr_ptr() const
    {
        return reinterpret_cast<const sockaddr*>(&addr);
    }
    socklen_t size() const
    {
        return len;
    }
    int domain() const
    {
        return AF_INET;
    }
};

class IPv6Address : public Address
{
   public:
    sockaddr_in6 addr{};
    socklen_t len = sizeof(addr);
    char addrStr[INET6_ADDRSTRLEN];

    IPv6Address(const std::string& ip, uint16_t port)
    {
        addr.sin6_family = AF_INET6;
        addr.sin6_port = htons(port);
        if (::inet_pton(AF_INET6, ip.c_str(), &addr.sin6_addr) <= 0)
            LOG_AND_THROW("Invalid IPv6 address");
        identifier = ip + ":" + std::to_string(port);
    }

    IPv6Address(const in6_addr& ip, uint16_t port)
    {
        addr.sin6_family = AF_INET6;
        addr.sin6_port = htons(port);
        addr.sin6_addr = ip;
        inet_ntop(AF_INET6, &addr.sin6_addr, addrStr, INET6_ADDRSTRLEN);
        identifier = "[" + std::string(addrStr) +
                     "]:" + std::to_string(ntohs(addr.sin6_port));
    }

    IPv6Address(const sockaddr* address)
    {
        addr = *reinterpret_cast<const sockaddr_in6*>(address);
        inet_ntop(AF_INET6, &addr.sin6_addr, addrStr, INET6_ADDRSTRLEN);
        identifier = "IPv6: [" + std::string(addrStr) +
                     "]:" + std::to_string(ntohs(addr.sin6_port));
    }

    const sockaddr* sockaddr_ptr() const
    {
        return reinterpret_cast<const sockaddr*>(&addr);
    }
    socklen_t size() const
    {
        return len;
    }
    int domain() const
    {
        return AF_INET6;
    }
};