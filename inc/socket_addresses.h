#ifndef __SOCKET_ADDRESSES_H_
#define __SOCKET_ADDRESSES_H_

#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <cstring>

struct Address {
    std::string identifier;
    virtual ~Address() = default;
    virtual const sockaddr* sockaddr_ptr() const = 0;
    virtual socklen_t size() const = 0;
    virtual int domain() const = 0;
};

struct UnixAddress : public Address {
    sockaddr_un addr{};
    socklen_t len = sizeof(addr);

    explicit UnixAddress(const std::string& path) {
        addr.sun_family = AF_UNIX;
        if (path.size() >= sizeof(addr.sun_path))
            throw std::runtime_error("Unix path too long");
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
        identifier = path;
    }

    UnixAddress(const sockaddr* address) {
        addr = *reinterpret_cast<const sockaddr_un*>(address);
        identifier = addr.sun_path;
    }

    const sockaddr* sockaddr_ptr() const { return reinterpret_cast<const sockaddr*>(&addr); }
    socklen_t size() const { return len; }
    int domain() const { return AF_UNIX; }
};

struct IPv4Address : public Address {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    char addrStr[INET_ADDRSTRLEN];

    IPv4Address(const std::string& ip, uint16_t port) {
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0)
            throw std::runtime_error("Invalid IPv4 address");
        identifier = ip + ":" + std::to_string(port);
    }

    IPv4Address(in_addr_t ip, uint16_t port) {
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = ip;
        inet_ntop(AF_INET, &addr.sin_addr.s_addr, addrStr, INET_ADDRSTRLEN);
        identifier = std::string(addrStr) + ":" + std::to_string(ntohs(addr.sin_port));
    }

    IPv4Address(const sockaddr* address) {
        addr = *reinterpret_cast<const sockaddr_in*>(address);
        inet_ntop(AF_INET, &addr.sin_addr.s_addr, addrStr, INET_ADDRSTRLEN);
        identifier = std::string(addrStr) + ":" + std::to_string(ntohs(addr.sin_port));
    }

    const sockaddr* sockaddr_ptr() const { return reinterpret_cast<const sockaddr*>(&addr); }
    socklen_t size() const { return len; }
    int domain() const { return AF_INET; }
};

struct IPv6Address : public Address {
    sockaddr_in6 addr{};
    socklen_t len = sizeof(addr);
    char addrStr[INET6_ADDRSTRLEN];

    IPv6Address(const std::string& ip, uint16_t port) {
        addr.sin6_family = AF_INET6;
        addr.sin6_port = htons(port);
        if (::inet_pton(AF_INET6, ip.c_str(), &addr.sin6_addr) <= 0)
            throw std::runtime_error("Invalid IPv6 address");
        identifier = ip + ":" + std::to_string(port);
    }

    IPv6Address(const in6_addr& ip, uint16_t port) {
        addr.sin6_family = AF_INET;
        addr.sin6_port = htons(port);
        addr.sin6_addr = ip;
        inet_ntop(AF_INET, &addr.sin6_addr, addrStr, INET6_ADDRSTRLEN);
        identifier = std::string(addrStr) + ":" + std::to_string(ntohs(addr.sin6_port));
    }

    IPv6Address(const sockaddr* address) {
        addr = *reinterpret_cast<const sockaddr_in6*>(address);
        inet_ntop(AF_INET, &addr.sin6_addr, addrStr, INET6_ADDRSTRLEN);
        identifier = std::string(addrStr) + ":" + std::to_string(ntohs(addr.sin6_port));
    }

    const sockaddr* sockaddr_ptr() const { return reinterpret_cast<const sockaddr*>(&addr); }
    socklen_t size() const { return len; }
    int domain() const { return AF_INET6; }
};

#endif // __SOCKET_ADDRESSES_