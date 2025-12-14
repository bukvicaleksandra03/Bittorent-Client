#include "socket.h"

#include <sys/types.h>

std::vector<std::unique_ptr<Address>> dns_lookup(const std::string& hostname,
                                                 const std::string& port)
{
    struct addrinfo hints
    {
    }, *res, *p;

    hints.ai_family = AF_UNSPEC;      // Allow both IPv4 and IPv6
    hints.ai_socktype = SOCK_STREAM;  // TCP socket

    int status = getaddrinfo(hostname.c_str(), port.c_str(), &hints, &res);
    if (status != 0)
    {
        throw std::runtime_error("getaddrinfo() failed");
    }

    std::vector<std::unique_ptr<Address>> addresses;

    // Loop through the results
    for (p = res; p != nullptr; p = p->ai_next)
    {
        if (p->ai_family == AF_INET)
        {
            std::unique_ptr<Address> address =
                std::make_unique<IPv4Address>(p->ai_addr);
            addresses.push_back(std::move(address));
        }
        else if (p->ai_family == AF_INET6)
        {
            std::unique_ptr<Address> address =
                std::make_unique<IPv6Address>(p->ai_addr);
            addresses.push_back(std::move(address));
        }
    }

    freeaddrinfo(res);  // Don't forget to free!

    return addresses;
}

Socket::Socket(int domain)
{
    if (domain != AF_UNIX && domain != AF_INET && domain != AF_INET6)
    {
        throw std::runtime_error(
            "Domain has to be one of these three options: AF_UNIX, AF_INET, "
            "AF_INET6.");
    }
    s_sockfd = ::socket(domain, SOCK_STREAM, 0);
    if (s_sockfd == -1)
    {
        throw std::runtime_error("Failed to create socket");
    }
    s_domain = domain;
}

Socket::Socket(int domain, int sockfd)
{
    if (domain != AF_UNIX && domain != AF_INET && domain != AF_INET6)
    {
        throw std::runtime_error(
            "Domain has to be one of these three options: AF_UNIX, AF_INET, "
            "AF_INET6.");
    }
    s_sockfd = sockfd;
    s_domain = domain;
}

Socket::Socket(Socket&& other) noexcept
    : s_sockfd(other.s_sockfd),
      s_domain(other.s_domain),
      s_backlog(other.s_backlog),
      s_addr(std::move(other.s_addr))
{
    other.s_sockfd = -1;
}

Socket& Socket::operator=(Socket&& other) noexcept
{
    if (this != &other)
    {
        if (s_sockfd != -1)
        {
            ::close(s_sockfd);
        }
        s_sockfd = other.s_sockfd;
        s_domain = other.s_domain;
        s_backlog = other.s_backlog;
        s_addr = std::move(other.s_addr);

        other.s_sockfd = -1;
    }
    return *this;
}

void Socket::bind(Address& address)
{
    if (address.domain() != s_domain)
        throw std::runtime_error(
            "You are trying to bind to an address of wrong domain.");

    if (::bind(s_sockfd, address.sockaddr_ptr(), address.size()) < 0)
        throw std::runtime_error("bind() failed");
}

void Socket::connect(Address& address)
{
    if (address.domain() != s_domain)
        throw std::runtime_error(
            "You are trying to connect to an address of wrong domain.");

    if (::connect(s_sockfd, address.sockaddr_ptr(), address.size()) < 0)
        throw std::runtime_error("connect() failed");
}

void Socket::listen(int backlog)
{
    if (::listen(s_sockfd, backlog) < 0)
        throw std::runtime_error("listen() failed");
    s_backlog = backlog;
}

Socket Socket::accept()
{
    struct sockaddr address;
    int addrlen = sizeof(address);

    Socket socket(s_domain, ::accept(s_sockfd, &address, (socklen_t*)&addrlen));

    std::unique_ptr<Address> addr =
        std::make_unique<IPv4Address>(reinterpret_cast<sockaddr*>(&address));
    socket.set_address(std::move(addr));

    return socket;
}

void Socket::set_address(std::unique_ptr<Address> address)
{
    s_addr = std::move(address);
}

const Address& Socket::get_address() const
{
    if (!s_addr)
        throw std::runtime_error("Socket has no address");
    return *s_addr;
}

Socket::~Socket()
{
    if (s_sockfd != -1)
    {
        ::close(s_sockfd);
    }
}

void Socket::send(const char* buffer, size_t size)
{
    ssize_t bytes_sent = ::send(s_sockfd, buffer, size, 0);
    if (bytes_sent <= 0)
        throw std::runtime_error("send() failed");
    while (static_cast<size_t>(bytes_sent) < size)
    {
        bytes_sent +=
            ::send(s_sockfd, buffer + bytes_sent, size - bytes_sent, 0);
        if (bytes_sent <= 0)
            throw std::runtime_error("send() failed");
    }
}

ssize_t Socket::recv(void* buffer, size_t size)
{
    ssize_t bytes_recv = ::recv(s_sockfd, buffer, size, 0);
    if (bytes_recv < 0)
        throw std::runtime_error("recv() failed");
    return bytes_recv;
}

std::string Socket::recv_all()
{
    std::string result;
    char buffer[4096];
    ssize_t bytes_read;

    while ((bytes_read = recv(buffer, sizeof(buffer))) > 0)
    {
        result.append(buffer, bytes_read);
    }

    return result;
}