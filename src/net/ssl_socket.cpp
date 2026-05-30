#include "net/ssl_socket.h"

#include <fcntl.h>
#include <poll.h>

#include <stdexcept>

#include "net/socket.h"

// Static flag to track if OpenSSL has been initialized
static bool ssl_initialized = false;

// One-time OpenSSL library initialization.
// SSL_library_init()           - registers all ciphers and digest algorithms
// SSL_load_error_strings()     - loads human-readable error messages
// OpenSSL_add_all_algorithms() - registers all available crypto algorithms
// Note: these calls are deprecated in OpenSSL 1.1.0+ (auto-initialized),
// but are kept for compatibility with older versions.
static void init_openssl()
{
    if (!ssl_initialized)
    {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
        ssl_initialized = true;
    }
}

// Retrieves the most recent error from OpenSSL's per-thread error queue
// and returns it as a human-readable string.
// ERR_get_error() pops one error off the queue (returns 0 if empty).
// ERR_error_string_n() converts the numeric code into a descriptive message.
static std::string get_ssl_error()
{
    unsigned long err = ERR_get_error();
    if (err == 0)
        return "Unknown SSL error";

    char buf[256];
    ERR_error_string_n(err, buf, sizeof(buf));
    return std::string(buf);
}

SSLSocket::SSLSocket(TCPClientSocket&& socket, const std::string& hostname)
    : _socket(std::move(socket))
{
    init_openssl();

    // Allocates and initializes an SSL context using TLS client method.
    _ctx = SSL_CTX_new(TLS_client_method());
    if (!_ctx)
        throw std::runtime_error("SSL_CTX_new() failed: " + get_ssl_error());

    // Create SSL connection object
    _ssl = SSL_new(_ctx);
    if (!_ssl)
    {
        SSL_CTX_free(_ctx);
        throw std::runtime_error("SSL_new() failed: " + get_ssl_error());
    }

    // Set SNI hostname (required by many servers).
    // SNI tells the server which domain the client is trying to reach. This is
    // necessary because a single IP address can host multiple HTTPS sites, and
    // the server needs to know which TLS certificate to present.
    if (!SSL_set_tlsext_host_name(_ssl, hostname.c_str()))
    {
        SSL_free(_ssl);
        SSL_CTX_free(_ctx);
        throw std::runtime_error("SSL_set_tlsext_host_name() failed");
    }

    // Attach SSL to the socket file descriptor
    if (!SSL_set_fd(_ssl, _socket.get_fd()))
    {
        SSL_free(_ssl);
        SSL_CTX_free(_ctx);
        throw std::runtime_error("SSL_set_fd() failed: " + get_ssl_error());
    }

    // Perform TLS handshake
    int result = SSL_connect(_ssl);
    if (result <= 0)
    {
        int err = SSL_get_error(_ssl, result);
        std::string error_msg = "SSL_connect() failed: ";

        switch (err)
        {
            case SSL_ERROR_SYSCALL:
                error_msg += "System call error";
                break;
            case SSL_ERROR_SSL:
                error_msg += get_ssl_error();
                break;
            default:
                error_msg += "Error code " + std::to_string(err);
        }

        SSL_free(_ssl);
        SSL_CTX_free(_ctx);
        throw std::runtime_error(error_msg);
    }
}

SSLSocket::SSLSocket(TCPAcceptSocket&& socket,
                     const std::string& cert_file,
                     const std::string& key_file)
    : _socket(std::move(socket))
{
    init_openssl();

    // Create a TLS server context (as opposed to TLS_client_method for
    // clients). This configures the SSL_CTX to perform the server side of the
    // TLS handshake.
    _ctx = SSL_CTX_new(TLS_server_method());
    if (!_ctx)
        throw std::runtime_error("SSL_CTX_new() failed: " + get_ssl_error());

    // Load the server's certificate from a PEM file.
    // The certificate is what the server presents to the client during the
    // TLS handshake so the client can verify the server's identity.
    if (SSL_CTX_use_certificate_file(
            _ctx, cert_file.c_str(), SSL_FILETYPE_PEM) <= 0)
    {
        SSL_CTX_free(_ctx);
        throw std::runtime_error("SSL_CTX_use_certificate_file() failed: " +
                      get_ssl_error());
    }

    // Load the server's private key from a PEM file.
    // The private key is used during the handshake to prove ownership of the
    // certificate. It must correspond to the public key in the certificate.
    if (SSL_CTX_use_PrivateKey_file(_ctx, key_file.c_str(), SSL_FILETYPE_PEM) <=
        0)
    {
        SSL_CTX_free(_ctx);
        throw std::runtime_error("SSL_CTX_use_PrivateKey_file() failed: " +
                      get_ssl_error());
    }

    // Verify that the loaded private key matches the loaded certificate.
    // If someone accidentally provides mismatched files, this catches it early
    // rather than failing with a cryptic error during the handshake.
    if (!SSL_CTX_check_private_key(_ctx))
    {
        SSL_CTX_free(_ctx);
        throw std::runtime_error("Private key does not match certificate: " +
                      get_ssl_error());
    }

    // Create a new SSL connection object from the context
    _ssl = SSL_new(_ctx);
    if (!_ssl)
    {
        SSL_CTX_free(_ctx);
        throw std::runtime_error("SSL_new() failed: " + get_ssl_error());
    }

    // Bind the SSL object to the accepted socket's file descriptor
    if (!SSL_set_fd(_ssl, _socket.get_fd()))
    {
        SSL_free(_ssl);
        SSL_CTX_free(_ctx);
        throw std::runtime_error("SSL_set_fd() failed: " + get_ssl_error());
    }

    // Perform the server side of the TLS handshake.
    // SSL_accept() waits for the client to initiate the handshake
    // (ClientHello), then responds with the server's certificate and negotiates
    // cipher/keys. This is the mirror of SSL_connect() in the client
    // constructor.
    int result = SSL_accept(_ssl);
    if (result <= 0)
    {
        int err = SSL_get_error(_ssl, result);
        std::string error_msg = "SSL_accept() failed: ";

        switch (err)
        {
            case SSL_ERROR_SYSCALL:
                error_msg += "System call error";
                break;
            case SSL_ERROR_SSL:
                error_msg += get_ssl_error();
                break;
            default:
                error_msg += "Error code " + std::to_string(err);
        }

        SSL_free(_ssl);
        SSL_CTX_free(_ctx);
        throw std::runtime_error(error_msg);
    }
}

SSLSocket::~SSLSocket()
{
    if (_ssl)
    {
        SSL_shutdown(_ssl);
        SSL_free(_ssl);
    }
    if (_ctx)
    {
        SSL_CTX_free(_ctx);
    }
}

SSLSocket::SSLSocket(SSLSocket&& other) noexcept
    : _socket(std::move(other._socket)), _ctx(other._ctx), _ssl(other._ssl)
{
    other._ctx = nullptr;
    other._ssl = nullptr;
}

SSLSocket& SSLSocket::operator=(SSLSocket&& other) noexcept
{
    if (this != &other)
    {
        // Clean up current resources
        if (_ssl)
        {
            SSL_shutdown(_ssl);
            SSL_free(_ssl);
        }
        if (_ctx)
        {
            SSL_CTX_free(_ctx);
        }

        // Move from other
        _socket = std::move(other._socket);
        _ctx = other._ctx;
        _ssl = other._ssl;

        other._ctx = nullptr;
        other._ssl = nullptr;
    }
    return *this;
}

void SSLSocket::send(const char* buffer, size_t size)
{
    size_t total_written = 0;
    while (total_written < size)
    {
        int ret = SSL_write(_ssl,
                            buffer + total_written,
                            static_cast<int>(size - total_written));
        if (ret > 0)
        {
            total_written += static_cast<size_t>(ret);
            continue;
        }
        int err = SSL_get_error(_ssl, ret);

        throw std::runtime_error("SSL_write() failed: error code " +
                      std::to_string(err) + " - " + get_ssl_error());
    }
}

ssize_t SSLSocket::recv(void* buffer, size_t size)
{
    int bytes_read = SSL_read(_ssl, buffer, static_cast<int>(size));

    if (bytes_read > 0)
    {
        return bytes_read;
    }

    if (bytes_read == 0)
    {
        // Connection closed cleanly
        return 0;
    }

    // bytes_read < 0, check the error
    int err = SSL_get_error(_ssl, bytes_read);

    if (err == SSL_ERROR_ZERO_RETURN)
    {
        // Peer closed connection
        return 0;
    }

    throw std::runtime_error("SSL_read() failed: error code " + std::to_string(err) +
                  " - " + get_ssl_error());
}

std::string SSLSocket::recv_all()
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

static void poll_or_throw(int fd,
                          short events,
                          int timeout_ms,
                          const char* op_name)
{
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = events;

    int ret = poll(&pfd, 1, timeout_ms);
    if (ret < 0)
        throw std::runtime_error(std::string(op_name) + " poll() failed: " +
                      strerror(errno));
    if (ret == 0)
        throw std::runtime_error(std::string(op_name) + " timed out after " +
                      std::to_string(timeout_ms) + "ms");
}

void SSLSocket::send_with_timeout(const char* buffer,
                                  size_t size,
                                  int timeout_ms)
{
    int fd = _socket.get_fd();
    // Set non-blocking for the duration of this call
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        throw std::runtime_error("fcntl(F_GETFL) failed: " +
                      std::string(strerror(errno)));
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        throw std::runtime_error("fcntl(F_SETFL) failed: " +
                      std::string(strerror(errno)));
    size_t total_written = 0;
    while (total_written < size)
    {
        int ret = SSL_write(_ssl,
                            buffer + total_written,
                            static_cast<int>(size - total_written));
        if (ret > 0)
        {
            total_written += static_cast<size_t>(ret);
            continue;
        }
        int err = SSL_get_error(_ssl, ret);
        if (err == SSL_ERROR_WANT_WRITE)
        {
            poll_or_throw(fd, POLLOUT, timeout_ms, "SSL_write()");
            continue;
        }
        if (err == SSL_ERROR_WANT_READ)
        {
            poll_or_throw(fd, POLLIN, timeout_ms, "SSL_write()");
            continue;
        }
        // Restore blocking before throwing
        fcntl(fd, F_SETFL, flags);
        throw std::runtime_error("SSL_write() failed: error code " +
                      std::to_string(err) + " - " + get_ssl_error());
    }
    // Restore blocking mode
    fcntl(fd, F_SETFL, flags);
}

ssize_t SSLSocket::recv_with_timeout(void* buffer, size_t size, int timeout_ms)
{
    int fd = _socket.get_fd();

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        throw std::runtime_error("fcntl(F_GETFL) failed: " +
                      std::string(strerror(errno)));
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        throw std::runtime_error("fcntl(F_SETFL) failed: " +
                      std::string(strerror(errno)));

    while (true)
    {
        if (SSL_pending(_ssl) == 0)
            poll_or_throw(fd, POLLIN, timeout_ms, "SSL_read()");

        int ret = SSL_read(_ssl, buffer, static_cast<int>(size));

        if (ret > 0)
        {
            fcntl(fd, F_SETFL, flags);
            return ret;
        }

        if (ret == 0)
        {
            fcntl(fd, F_SETFL, flags);
            return 0;
        }

        int err = SSL_get_error(_ssl, ret);

        if (err == SSL_ERROR_ZERO_RETURN)
        {
            fcntl(fd, F_SETFL, flags);
            return 0;
        }
        if (err == SSL_ERROR_WANT_READ)
            continue;
        if (err == SSL_ERROR_WANT_WRITE)
        {
            poll_or_throw(fd, POLLOUT, timeout_ms, "SSL_read()");
            continue;
        }

        fcntl(fd, F_SETFL, flags);
        throw std::runtime_error("SSL_read() failed: error code " + std::to_string(err) +
                      " - " + get_ssl_error());
    }
}