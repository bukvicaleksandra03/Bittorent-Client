#pragma once

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <string>

#include "socket.h"

class SSLSocket
{
   public:
    SSLSocket(Socket&& socket, const std::string& hostname);
    ~SSLSocket();

    // Delete copy
    SSLSocket(const SSLSocket&) = delete;
    SSLSocket& operator=(const SSLSocket&) = delete;

    // Move
    SSLSocket(SSLSocket&& other) noexcept;
    SSLSocket& operator=(SSLSocket&& other) noexcept;

    void send(const char* buffer, size_t size);

    ssize_t recv(void* buffer, size_t size);

    // Reads until connection closes, returns complete response
    std::string recv_all();

   private:
    Socket _socket;
    SSL_CTX* _ctx = nullptr;
    SSL* _ssl = nullptr;
};