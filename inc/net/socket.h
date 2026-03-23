#pragma once

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <memory>
#include <vector>

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

    void send(const char* buffer, size_t size);

    ssize_t recv(void* buffer, size_t size);

    // Reads until connection closes, returns complete response
    std::string recv_all();

    // Set read/write timeout in seconds
    void set_timeout(int seconds);

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

class AcceptSocket;
// Also known as Passive Sockets
class ServerSocket: public Socket 
{
  public:
    explicit ServerSocket(int domain, int socket_type);

    // Move constructor and operator
    ServerSocket(ServerSocket&& other) noexcept;
    ServerSocket& operator=(ServerSocket&& other) noexcept;

    void bind(Address& address);

    void listen(int backlog);

    AcceptSocket accept();
  private:

    // It specifies the limit of pending connections to this socket.
    int s_backlog;
};

// Socket which is created on the server when a connection is accepted
class AcceptSocket: public Socket
{
  public:
    explicit AcceptSocket(int domain, int sockfd, int socket_type);

};

// Also known as Active Sockets
class ClientSocket: public Socket
{
  public:
    explicit ClientSocket(int domain, int socket_type);

    void connect(Address& address);

    void connect_with_timeout(Address& address, int timeout_ms);

};