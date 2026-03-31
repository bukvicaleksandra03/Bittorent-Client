#pragma once

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <string>

#include "net/socket.h"

enum class SSLMode
{
    Client,
    Server
};

// SSL - Secure Sockets Layer
// TLS - Transport Layer Security - successor to SSL

// SSLSocket is a wrapper class that takes an existing raw TCP Socket and
// upgrades it to a TLS-encrypted connection using OpenSSL. Think of it as a
// layer that sits on top of your plain Socket -- after construction, all data
// sent and received through it is automatically encrypted/decrypted.
class SSLSocket
{
   public:
    // Client constructor - connects to a remote server
    SSLSocket(TCPClientSocket&& socket, const std::string& hostname);

    // Server constructor - accepts an incoming TLS connection
    // cert_file: path to PEM certificate (e.g., "server.crt")
    // key_file:  path to PEM private key (e.g., "server.key")
    SSLSocket(TCPAcceptSocket&& socket,
              const std::string& cert_file,
              const std::string& key_file);

    ~SSLSocket();

    // Delete copy
    SSLSocket(const SSLSocket&) = delete;
    SSLSocket& operator=(const SSLSocket&) = delete;

    // Move
    SSLSocket(SSLSocket&& other) noexcept;
    SSLSocket& operator=(SSLSocket&& other) noexcept;

    void send(const char* buffer, size_t size);
    void send_with_timeout(const char* buffer, size_t size, int timeout_ms);

    ssize_t recv(void* buffer, size_t size);
    ssize_t recv_with_timeout(void* buffer, size_t size, int timeout_ms);

    std::string recv_all();

   private:
    TCPSocket _socket;
    SSL_CTX* _ctx = nullptr;
    SSL* _ssl = nullptr;
};