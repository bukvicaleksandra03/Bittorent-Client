#pragma once

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "net/socket_addresses.h"

std::vector<std::unique_ptr<Address>> dns_lookup(const std::string& hostname,
                                                 const std::string& port,
                                                 int socket_type);

// TCPSocket(fd, domain, destructor, move)
// ├── TCPServerSocket(bind, listen, accept)
// └── TCPDataSocket : TCPSocket(send, recv, recv_all + timeout variants)
//     ├── TCPAcceptSocket
//     └── TCPClientSocket(connect, connect_with_timeout)

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

    // Used when creating an SSL socket
    int get_fd() const;

    virtual ~TCPSocket();

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

    // -----------------------------------------------------------------------
    // Choosing the right TCP recv call
    //
    //  Function                   Blocks until          Throws when
    //  ─────────────────────────────────────────────────────────────────────
    //  recv(buf, n)               ≥1 byte ready         system error
    //  recv_with_timeout(…, ms)   ≥1 byte OR timeout    timeout / sys error
    //  recv_all()                 connection closes      sys error
    //  recv_all_with_timeout(ms)  connection closes      timeout / sys error
    //  recv_exact(buf, n)         exactly n bytes        EOF / sys error
    //  recv_exact_timeout(…, ms)  exactly n bytes        EOF / timeout / err
    //
    // recv / recv_with_timeout: single-call; caller must loop if more is
    // needed. recv_all*: for protocols where connection close means
    // end-of-message
    //            (e.g. HTTP/1.0). Hangs on persistent connections.
    // recv_exact*: for length-prefixed framing (e.g. BitTorrent peer wire).
    //              Throws on unexpected EOF, so the caller never gets partial
    //              data.
    // -----------------------------------------------------------------------

    ssize_t recv(void* buffer, size_t size);
    ssize_t recv_with_timeout(void* buffer, size_t size, int timeout_ms);
    std::string recv_all();
    std::string recv_all_with_timeout(int timeout_ms);
    void recv_exact(uint8_t* buf, size_t n);
    void recv_exact_timeout(uint8_t* buf, size_t n, int timeout_ms);

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

    void bind(const Address& address);

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

    void connect(const Address& address);

    void connect_with_timeout(const Address& address, int timeout_ms);
};

// UDPSocket(fd, domain, destructor, move, bind, connect, sendto, recvfrom,
//           local_address)

class UDPSocket
{
   public:
    static const int s_socket_type = SOCK_DGRAM;

    UDPSocket() = delete;

    explicit UDPSocket(int domain);

    // Delete copy to avoid double-close
    UDPSocket(const UDPSocket&) = delete;
    UDPSocket& operator=(const UDPSocket&) = delete;

    // Move constructor and operator
    UDPSocket(UDPSocket&& other) noexcept;
    UDPSocket& operator=(UDPSocket&& other) noexcept;

    ~UDPSocket();

    int get_fd() const;

    // Close the underlying fd and invalidate this socket.  Safe to call
    // multiple times.  The destructor will not close again.
    void close();

    static constexpr size_t MAX_UDP_PAYLOAD = 65507;

    void bind(const Address& address);

    // Records the given address as the socket's remote peer.
    //
    // On UDP, connect() does NOT send any packet and does NOT complete a
    // handshake — it simply stores the destination in the kernel's socket
    // state.  Two useful side-effects follow:
    //   1. Subsequent send()/recv() calls may omit the address (the kernel
    //      fills it in automatically).
    //   2. The kernel performs a routing-table lookup to pick the outgoing
    //      interface and source IP.  Once connect() returns, local_address()
    //      will reveal that source IP — which is otherwise unknowable without
    //      walking /proc/net/route manually.
    void connect(const Address& address);

    std::pair<std::string, std::unique_ptr<Address>> recvfrom();

    // Waits up to timeout_ms for a datagram to arrive, then calls recvfrom().
    // Throws if the timeout expires before a datagram is available.
    std::pair<std::string, std::unique_ptr<Address>> recvfrom_with_timeout(
        int timeout_ms);

    void sendto(const std::string& message, const Address& dst_address);

    // Returns the local address currently assigned to this socket, as
    // reported by getsockname().  Useful after bind() (to discover the
    // ephemeral port chosen by the OS) or after connect() (to discover
    // which source IP the kernel selected for the given destination).
    // The concrete type matches the socket's domain: IPv4Address for
    // AF_INET, IPv6Address for AF_INET6.
    std::unique_ptr<Address> local_address() const;

    // Multicast helpers (IPv4 only; silently ignored on other socket types).
    //
    // set_multicast_ttl  – controls how many router hops a multicast packet
    //   may traverse.  1 = LAN only; 4 = conventional SSDP default.
    //
    // set_multicast_if   – forces the outgoing interface for multicast
    //   traffic.  Pass either the sin_addr of the desired local interface or
    //   its dotted-decimal string (e.g. "192.168.1.10").
    void set_multicast_ttl(int ttl);
    void set_multicast_if(in_addr iface);
    void set_multicast_if(const std::string& iface_ip);

   private:
    int s_sockfd = -1;
    int s_domain;  // AF_UNIX, AF_INET or AF_INET6
};