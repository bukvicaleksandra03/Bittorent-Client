#include "socket.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>

#include <cstring>

#include "logger.h"

std::vector<std::unique_ptr<Address>> dns_lookup(const std::string& hostname,
                                                 const std::string& port,
                                                 int socket_type)
{
    if (socket_type != SOCK_STREAM && socket_type != SOCK_DGRAM)
    {
        throw std::runtime_error(
            "Socket type has to be one of these two options: SOCK_STREAM "
            "(TCP), SOCK_DGRAM (UDP).");
    }

    struct addrinfo hints
    {
    }, *res, *p;

    hints.ai_family = AF_UNSPEC;      // Allow both IPv4 and IPv6
    hints.ai_socktype = socket_type;  // TCP or UDP

    LOG_D("DNS lookup for " + hostname + ":" + port + " with socket type " +
          socket_type_to_string(socket_type) + ".");
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
            auto address = std::make_unique<IPv4Address>(p->ai_addr);
            LOG_D("Found IPv4 address: " + address->identifier);
            addresses.push_back(std::move(address));
        }
        else if (p->ai_family == AF_INET6)
        {
            auto address = std::make_unique<IPv6Address>(p->ai_addr);
            LOG_D("Found IPv6 address: " + address->identifier);
            addresses.push_back(std::move(address));
        }
    }

    freeaddrinfo(res);

    return addresses;
}

Socket::Socket(int domain, int socket_type)
{
    if (domain != AF_UNIX && domain != AF_INET && domain != AF_INET6)
    {
        throw std::runtime_error(
            "Domain has to be one of these three options: AF_UNIX, AF_INET, "
            "AF_INET6.");
    }
    if (socket_type != SOCK_STREAM && socket_type != SOCK_DGRAM)
    {
        throw std::runtime_error(
            "Socket type has to be one of these two options: SOCK_STREAM "
            "(TCP), SOCK_DGRAM (UDP).");
    }
    s_sockfd = ::socket(domain, socket_type, 0);
    if (s_sockfd == -1)
    {
        throw std::runtime_error("Failed to create socket");
    }
    s_domain = domain;
    s_socket_type = socket_type;
}

Socket::Socket(int domain, int sockfd, int socket_type)
{
    if (domain != AF_UNIX && domain != AF_INET && domain != AF_INET6)
    {
        throw std::runtime_error(
            "Domain has to be one of these three options: AF_UNIX, AF_INET, "
            "AF_INET6.");
    }
    s_sockfd = sockfd;
    s_domain = domain;
    s_socket_type = socket_type;
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
        throw std::runtime_error("connect() failed: " +
                                 std::string(strerror(errno)));
}

void Socket::connect_with_timeout(Address& address, int timeout_ms)
{
    if (address.domain() != s_domain)
        throw std::runtime_error(
            "You are trying to connect to an address of wrong domain.");

    // Set socket to non-blocking for connection with timeout
    int flags = fcntl(s_sockfd, F_GETFL, 0);
    fcntl(s_sockfd, F_SETFL, flags | O_NONBLOCK);

    int result = ::connect(s_sockfd, address.sockaddr_ptr(), address.size());

    if (result < 0)
    {
        if (errno == EINPROGRESS)
        {
            // Connection in progress, wait with timeout
            struct pollfd pfd;
            pfd.fd = s_sockfd;
            pfd.events = POLLOUT;

            int poll_result = poll(&pfd, 1, timeout_ms);

            if (poll_result == 0)
            {
                // Restore blocking mode before throwing
                fcntl(s_sockfd, F_SETFL, flags);
                throw std::runtime_error("connect() timed out");
            }
            else if (poll_result < 0)
            {
                fcntl(s_sockfd, F_SETFL, flags);
                throw std::runtime_error("poll() failed during connect");
            }

            // Check if connection succeeded
            int error = 0;
            socklen_t len = sizeof(error);
            getsockopt(s_sockfd, SOL_SOCKET, SO_ERROR, &error, &len);

            if (error != 0)
            {
                fcntl(s_sockfd, F_SETFL, flags);
                throw std::runtime_error("connect() failed: " +
                                         std::string(strerror(error)));
            }
        }
        else
        {
            fcntl(s_sockfd, F_SETFL, flags);
            throw std::runtime_error("connect() failed: " +
                                     std::string(strerror(errno)));
        }
    }

    // Restore blocking mode
    fcntl(s_sockfd, F_SETFL, flags);
}

void Socket::set_timeout(int seconds)
{
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;

    setsockopt(s_sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s_sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
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

    Socket socket(s_domain,
                  ::accept(s_sockfd, &address, (socklen_t*)&addrlen),
                  s_socket_type);

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