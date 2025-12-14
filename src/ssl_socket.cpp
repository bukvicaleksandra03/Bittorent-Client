#include "ssl_socket.h"

#include <stdexcept>

// Static flag to track if OpenSSL has been initialized
static bool ssl_initialized = false;

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

static std::string get_ssl_error()
{
    unsigned long err = ERR_get_error();
    if (err == 0)
        return "Unknown SSL error";

    char buf[256];
    ERR_error_string_n(err, buf, sizeof(buf));
    return std::string(buf);
}

SSLSocket::SSLSocket(Socket&& socket, const std::string& hostname)
    : _socket(std::move(socket))
{
    init_openssl();

    // Create SSL context using TLS client method
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

    // Set SNI hostname (required by many servers)
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
    int bytes_written = SSL_write(_ssl, buffer, static_cast<int>(size));
    if (bytes_written <= 0)
    {
        int err = SSL_get_error(_ssl, bytes_written);
        throw std::runtime_error("SSL_write() failed: error code " +
                                 std::to_string(err));
    }
    while (static_cast<size_t>(bytes_written) < size)
    {
        bytes_written += SSL_write(_ssl,
                                   buffer + bytes_written,
                                   static_cast<int>(size - bytes_written));
        if (bytes_written <= 0)
        {
            int err = SSL_get_error(_ssl, bytes_written);
            throw std::runtime_error("SSL_write() failed: error code " +
                                     std::to_string(err));
        }
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

    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
    {
        // Non-blocking would need retry, but we're blocking so treat as 0
        return 0;
    }

    throw std::runtime_error("SSL_read() failed: error code " +
                             std::to_string(err) + " - " + get_ssl_error());
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
