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
                                                 const std::string& port);

class Socket
{
   public:
    explicit Socket(int domain);

    explicit Socket(int domain, int sockfd);

    // Delete copy to avoid double-close
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    // Move constructor and operator
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    void bind(Address& address);

    void connect(Address& address);

    void listen(int backlog);

    Socket accept();

    void set_address(std::unique_ptr<Address> address);

    const Address& get_address() const;

    ssize_t read(void* buffer, int buffer_size);

    void send(const char* buffer, int buffer_size);

    ~Socket();

   private:
    // File descriptor of the socket
    int s_sockfd = -1;

    int s_domain;
    int s_backlog;
    std::unique_ptr<Address> s_addr;
};