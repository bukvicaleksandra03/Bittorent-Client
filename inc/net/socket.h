#pragma once

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <memory>
#include <string>
#include <vector>

#include "socket_addresses.h"

#define SV_SOCK_PATH "/tmp/unix_socket"

std::vector<std::unique_ptr<Address>> dns_lookup(const std::string& hostname,
                                                 const std::string& port,
                                                 int socket_type);

class TCPSocket
{
   public:
    static const int s_socket_type = SOCK_STREAM;

    TCPSocket() = delete;

    // Delete copy to avoid double-close
    TCPSocket(const TCPSocket&) = delete;
    TCPSocket& operator=(const TCPSocket&) = delete;

    // Move constructor and operator
    TCPSocket(TCPSocket&& other) noexcept;
    TCPSocket& operator=(TCPSocket&& other) noexcept;

    ~TCPSocket();

   protected:
    TCPSocket(int domain);

    int s_sockfd = -1;
    int s_domain;  // AF_UNIX, AF_INET or AF_INET6
};

// Intermediate base for sockets that send and receive data
class TCPDataSocket : public TCPSocket
{
   public:
    void send(const char* buffer, size_t size);
    void send_with_timeout(const char* buffer, size_t size, int timeout_ms);

    ssize_t recv(void* buffer, size_t size);
    ssize_t recv_with_timeout(void* buffer, size_t size, int timeout_ms);

    std::string recv_all();
    std::string recv_all_with_timeout(int timeout_ms);

   protected:
    explicit TCPDataSocket(int domain) : TCPSocket(domain) {}
};

class TCPAcceptSocket;
// Also known as Passive Sockets
class TCPServerSocket : public TCPSocket
{
   public:
    explicit TCPServerSocket(int domain);

    // Move constructor and operator
    TCPServerSocket(TCPServerSocket&& other) noexcept;
    TCPServerSocket& operator=(TCPServerSocket&& other) noexcept;

    void bind(Address& address);

    void listen(int backlog);

    TCPAcceptSocket accept();

   private:
    // The kernel must record some information about each pending connection
    // request so that a subsequent accept() can be processed. The backlog
    // argument allows us to limit the number of such pending connections.
    // Connection requests up to this limit succeed immediately. (For TCP
    // sockets, the story is a little more complicated, as we’ll see in
    // Section 61.6.4.) Further connection requests block until a pending
    // connection is accepted (via accept()), and thus removed from the queue of
    // pending connections
    int s_backlog;
};

// Socket which is created on the server when a connection is accepted
class TCPAcceptSocket : public TCPDataSocket
{
   public:
    explicit TCPAcceptSocket(int domain, int sockfd);
};

// Also known as Active Sockets
class TCPClientSocket : public TCPDataSocket
{
   public:
    explicit TCPClientSocket(int domain);

    void connect(Address& address);

    void connect_with_timeout(Address& address, int timeout_ms);
};

class UDPSocket
{
   public:
    static const int s_socket_type = SOCK_DGRAM;
};

class UDPServerSocket : public UDPSocket
{
};

class UDPClientSocket : public UDPSocket
{
};
`