#include "net/socket.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <cstring>
#include <string>

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

TCPSocket::TCPSocket(int domain)
{
    if (domain != AF_UNIX && domain != AF_INET && domain != AF_INET6)
    {
        throw std::runtime_error(
            "Domain has to be one of these three options: AF_UNIX, AF_INET, "
            "AF_INET6.");
    }

    s_domain = domain;
}

TCPSocket::TCPSocket(TCPSocket&& other) noexcept
    : s_sockfd(other.s_sockfd), s_domain(other.s_domain)
{
    other.s_sockfd = -1;
}

TCPSocket& TCPSocket::operator=(TCPSocket&& other) noexcept
{
    if (this != &other)
    {
        if (s_sockfd != -1)
        {
            ::close(s_sockfd);
        }
        s_sockfd = other.s_sockfd;
        s_domain = other.s_domain;

        other.s_sockfd = -1;
    }
    return *this;
}

static void poll_or_throw(int fd, short events, int timeout_ms,
                          const char* op_name)
{
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = events;

    int ret = poll(&pfd, 1, timeout_ms);
    if (ret < 0)
        throw std::runtime_error(std::string(op_name) +
                                 " poll() failed: " + strerror(errno));
    if (ret == 0)
        throw std::runtime_error(std::string(op_name) + " timed out after " +
                                 std::to_string(timeout_ms) + "ms");
}

TCPSocket::~TCPSocket()
{
    if (s_sockfd != -1)
    {
        // Subsequent reads/writes to the local socket yield the SIGPIPE signal
        // and an EPIPE error. This is also true for the peer socket.
        ::shutdown(s_sockfd, SHUT_RDWR);

        ::close(s_sockfd);
    }
}

void TCPDataSocket::send(const char* buffer, size_t size)
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

void TCPDataSocket::send_with_timeout(const char* buffer, size_t size,
                                      int timeout_ms)
{
    size_t total_sent = 0;
    while (total_sent < size)
    {
        poll_or_throw(s_sockfd, POLLOUT, timeout_ms, "send()");

        ssize_t n =
            ::send(s_sockfd, buffer + total_sent, size - total_sent, 0);
        if (n <= 0)
            throw std::runtime_error("send() failed: " +
                                     std::string(strerror(errno)));
        total_sent += static_cast<size_t>(n);
    }
}

ssize_t TCPDataSocket::recv(void* buffer, size_t size)
{
    ssize_t bytes_recv = ::recv(s_sockfd, buffer, size, 0);
    if (bytes_recv < 0)
        throw std::runtime_error("recv() failed");
    return bytes_recv;
}

ssize_t TCPDataSocket::recv_with_timeout(void* buffer, size_t size,
                                         int timeout_ms)
{
    poll_or_throw(s_sockfd, POLLIN, timeout_ms, "recv()");

    ssize_t bytes_recv = ::recv(s_sockfd, buffer, size, 0);
    if (bytes_recv < 0)
        throw std::runtime_error("recv() failed: " +
                                 std::string(strerror(errno)));
    return bytes_recv;
}

std::string TCPDataSocket::recv_all()
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

std::string TCPDataSocket::recv_all_with_timeout(int timeout_ms)
{
    std::string result;
    char buffer[4096];

    for (;;)
    {
        poll_or_throw(s_sockfd, POLLIN, timeout_ms, "recv_all()");

        ssize_t bytes_read = ::recv(s_sockfd, buffer, sizeof(buffer), 0);
        if (bytes_read < 0)
            throw std::runtime_error("recv() failed: " +
                                     std::string(strerror(errno)));
        if (bytes_read == 0)
            break;
        result.append(buffer, bytes_read);
    }

    return result;
}

TCPServerSocket::TCPServerSocket(int domain) : TCPSocket(domain)
{
    s_sockfd = ::socket(domain, s_socket_type, 0);
    if (s_sockfd == -1)
    {
        throw std::runtime_error("Failed to create socket");
    }
}

TCPServerSocket::TCPServerSocket(TCPServerSocket&& other) noexcept
    : TCPSocket(std::move(other)), s_backlog(other.s_backlog)
{
}

TCPServerSocket& TCPServerSocket::operator=(TCPServerSocket&& other) noexcept
{
    if (this != &other)
    {
        TCPSocket::operator=(std::move(other));
        s_backlog = other.s_backlog;
    }
    return *this;
}

void TCPServerSocket::bind(Address& address)
{
    if (address.domain() != s_domain)
    {
        throw std::runtime_error(
            "You are trying to bind to an address of wrong domain.");
    }

    if (::bind(s_sockfd, address.sockaddr_ptr(), address.size()) < 0)
    {
        throw std::runtime_error("bind() failed");
    }
}

void TCPServerSocket::listen(int backlog)
{
    if (::listen(s_sockfd, backlog) < 0)
        throw std::runtime_error("listen() failed");

    s_backlog = backlog;
}

TCPAcceptSocket::TCPAcceptSocket(int domain, int sockfd) : TCPDataSocket(domain)
{
    s_sockfd = sockfd;
}

TCPAcceptSocket TCPServerSocket::accept()
{
    struct sockaddr address;
    socklen_t addrlen = sizeof(address);

    int fd = ::accept(s_sockfd, &address, &addrlen);
    if (fd < 0)
    {
        throw std::runtime_error("accept() failed: " +
                                 std::string(strerror(errno)));
    }

    return TCPAcceptSocket(s_domain, fd);
}

TCPClientSocket::TCPClientSocket(int domain) : TCPDataSocket(domain)
{
    s_sockfd = ::socket(domain, s_socket_type, 0);
    if (s_sockfd == -1)
    {
        throw std::runtime_error("Failed to create socket");
    }
}

void TCPClientSocket::connect(Address& address)
{
    if (address.domain() != s_domain)
        throw std::runtime_error(
            "You are trying to connect to an address of wrong domain.");

    if (::connect(s_sockfd, address.sockaddr_ptr(), address.size()) < 0)
        throw std::runtime_error("connect() failed: " +
                                 std::string(strerror(errno)));
}

void TCPClientSocket::connect_with_timeout(Address& address, int timeout_ms)
{
    if (address.domain() != s_domain)
        throw std::runtime_error(
            "You are trying to connect to an address of wrong domain.");

    // Step 1: Save original flags and set non-blocking
    int flags = fcntl(s_sockfd, F_GETFL, 0);
    if (flags < 0)
        throw std::runtime_error("fcntl(F_GETFL) failed: " +
                                 std::string(strerror(errno)));

    if (fcntl(s_sockfd, F_SETFL, flags | O_NONBLOCK) < 0)
        throw std::runtime_error("fcntl(F_SETFL) failed: " +
                                 std::string(strerror(errno)));

    // Step 2: Initiate connect (returns immediately on non-blocking socket)
    int ret = ::connect(s_sockfd, address.sockaddr_ptr(), address.size());

    if (ret < 0 && errno != EINPROGRESS)
    {
        // Restore blocking mode before throwing
        fcntl(s_sockfd, F_SETFL, flags);
        throw std::runtime_error("connect() failed: " +
                                 std::string(strerror(errno)));
    }

    if (ret == 0)
    {
        // Connected immediately (e.g. localhost)
        fcntl(s_sockfd, F_SETFL, flags);
        return;
    }

    // Step 3: Wait for the socket to become writable (connection complete)
    struct pollfd pfd;
    pfd.fd = s_sockfd;
    pfd.events = POLLOUT;

    int poll_ret = poll(&pfd, 1, timeout_ms);

    if (poll_ret < 0)
    {
        fcntl(s_sockfd, F_SETFL, flags);
        throw std::runtime_error("poll() failed: " +
                                 std::string(strerror(errno)));
    }

    if (poll_ret == 0)
    {
        // Timed out
        fcntl(s_sockfd, F_SETFL, flags);
        throw std::runtime_error("connect() timed out after " +
                                 std::to_string(timeout_ms) + "ms");
    }

    // Step 4: Check if the connection actually succeeded
    int so_error = 0;
    socklen_t len = sizeof(so_error);
    if (getsockopt(s_sockfd, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0)
    {
        fcntl(s_sockfd, F_SETFL, flags);
        throw std::runtime_error("getsockopt(SO_ERROR) failed: " +
                                 std::string(strerror(errno)));
    }

    // Step 5: Restore blocking mode
    fcntl(s_sockfd, F_SETFL, flags);

    if (so_error != 0)
    {
        throw std::runtime_error("connect() failed: " +
                                 std::string(strerror(so_error)));
    }
}