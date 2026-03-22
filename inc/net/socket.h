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
    explicit Socket(int domain, int socket_type);

    explicit Socket(int domain, int sockfd, int socket_type);

    // Delete copy to avoid double-close
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    // Move constructor and operator
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    void bind(Address& address);

    void connect(Address& address);

    void connect_with_timeout(Address& address, int timeout_ms);

    void listen(int backlog);

    Socket accept();

    void set_address(std::unique_ptr<Address> address);

    const Address& get_address() const;

    void send(const char* buffer, size_t size);

    ssize_t recv(void* buffer, size_t size);

    // Reads until connection closes, returns complete response
    std::string recv_all();

    int get_fd() const
    {
        return s_sockfd;
    }

    // Set read/write timeout in seconds
    void set_timeout(int seconds);

    ~Socket();

   private:
    int s_socket_type;
    // File descriptor of the socket
    int s_sockfd = -1;

    int s_domain;
    int s_backlog;
    std::unique_ptr<Address> s_addr;
};