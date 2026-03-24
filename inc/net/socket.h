#pragma once

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <memory>
#include <vector>

#include "socket.h"
#include "socket_addresses.h"

#define SV_SOCK_PATH "/tmp/unix_socket"

std::vector<std::unique_ptr<Address>> dns_lookup(const std::string& hostname,
                                                 const std::string& port,
                                                 int socket_type);

class Socket
{
   public:
    Socket() = delete;

    // Delete copy to avoid double-close
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    // Move constructor and operator
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    // void send(const char* buffer, size_t size);

    // ssize_t recv(void* buffer, size_t size);

    // // Reads until connection closes, returns complete response
    // std::string recv_all();

    // // Set read/write timeout in seconds
    // void set_timeout(int seconds);

    ~Socket();

   protected:
    Socket(int domain, int socket_type);

    // File descriptor of the socket
    int s_sockfd = -1;

    int s_domain;       // AF_UNIX, AF_INET or AR_INET6
    int s_socket_type;  // SOCK_STREAM or SOCK_DGRAM

    void verify_domain(int domain);
    void verify_socket_type(int socket_type);
};

class TCPAcceptSocket;
// Also known as Passive Sockets
class TCPServerSocket : public Socket
{
   public:
    explicit TCPServerSocket(int domain, int socket_type);

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
class TCPAcceptSocket : public Socket
{
   public:
    explicit TCPAcceptSocket(int domain, int sockfd, int socket_type);
};

// Also known as Active Sockets
class TCPClientSocket : public Socket
{
   public:
    explicit TCPClientSocket(int domain, int socket_type);

    void connect(Address& address);

    void connect_with_timeout(Address& address, int timeout_ms);
};