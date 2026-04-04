#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <functional>
#include <string>
#include <thread>

#include "net/socket.h"
#include "net/socket_addresses.h"
#include "net/ssl_socket.h"

static const char* CERT_FILE = "tests/net/certs/server.crt";
static const char* KEY_FILE = "tests/net/certs/server.key";

struct IgnoreSigpipe
{
    IgnoreSigpipe()
    {
        struct sigaction sa = {};
        sa.sa_handler = SIG_IGN;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGPIPE, &sa, nullptr);
    }
};
static IgnoreSigpipe ignore_sigpipe;

// ---------------------------------------------------------------------------
// Fixture -- eliminates the repeated server-thread scaffolding
// ---------------------------------------------------------------------------
class SSLSocketTest : public ::testing::Test
{
  protected:
    std::thread server_thread_;
    std::atomic<bool> ready_{false};
    std::string server_error_;
    int port_ = 0;

    void SetUp() override
    {
        static std::atomic<int> port_counter{19100};
        port_ = ++port_counter;
        ready_ = false;
        server_error_.clear();
    }

    // Safety net: if the test exits early (e.g. ASSERT failure) and
    // join_server() was never called, we still join the thread so the
    // process doesn't call std::terminate.
    void TearDown() override
    {
        if (server_thread_.joinable())
            server_thread_.join();
    }

    // Starts a background server that listens, accepts one connection,
    // wraps it in TLS, and hands the SSLSocket to `handler`.
    // Any exception inside the thread is captured in server_error_.
    void start_server(std::function<void(SSLSocket)> handler)
    {
        server_thread_ = std::thread(
            [this, handler = std::move(handler)]()
            {
                try
                {
                    IPv4Address addr(INADDR_ANY, port_);
                    TCPServerSocket server(AF_INET);
                    int opt = 1;
                    setsockopt(server.get_fd(), SOL_SOCKET, SO_REUSEADDR,
                               &opt, sizeof(opt));
                    server.bind(addr);
                    server.listen(1);

                    ready_ = true;

                    TCPAcceptSocket accepted = server.accept();
                    SSLSocket ssl(std::move(accepted), CERT_FILE, KEY_FILE);
                    handler(std::move(ssl));
                }
                catch (const std::exception& e)
                {
                    server_error_ = e.what();
                }
            });
    }

    // Blocks until the server is listening, then connects a TLS client.
    SSLSocket connect_client()
    {
        while (!ready_)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));

        IPv4Address addr("127.0.0.1", port_);
        TCPClientSocket tcp(AF_INET);
        tcp.connect(addr);
        return SSLSocket(std::move(tcp), "localhost");
    }

    // Joins the server thread and asserts it finished without error.
    // Call this inside the test body BEFORE any captured locals go out
    // of scope, so the lambda can safely access them.
    void join_server()
    {
        if (server_thread_.joinable())
            server_thread_.join();
        ASSERT_TRUE(server_error_.empty()) << "Server error: " << server_error_;
    }
};

// ---------------------------------------------------------------------------
// Test 1: Client sends a message, server echoes back a response
// ---------------------------------------------------------------------------
TEST_F(SSLSocketTest, BasicSendReceive)
{
    start_server(
        [](SSLSocket ssl)
        {
            char buf[4096] = {0};
            ssl.recv(buf, sizeof(buf));

            const char* reply = "Hello from TLS server";
            ssl.send(reply, strlen(reply));
        });

    SSLSocket client = connect_client();
    const char* msg = "Hello from TLS client";
    client.send(msg, strlen(msg));

    char buf[4096] = {0};
    ssize_t n = client.recv(buf, sizeof(buf));

    join_server();

    EXPECT_GT(n, 0);
    EXPECT_STREQ(buf, "Hello from TLS server");
}

// ---------------------------------------------------------------------------
// Test 2: Send a large payload (100 KB) to exercise the send loop
// ---------------------------------------------------------------------------
TEST_F(SSLSocketTest, LargePayload)
{
    std::string received_data;

    start_server(
        [&received_data](SSLSocket ssl)
        {
            received_data = ssl.recv_all();
        });

    const size_t PAYLOAD_SIZE = 100 * 1024;
    std::string large(PAYLOAD_SIZE, 'A');

    {
        SSLSocket client = connect_client();
        client.send(large.c_str(), large.size());
    }

    join_server();

    EXPECT_EQ(received_data.size(), PAYLOAD_SIZE);
    EXPECT_EQ(received_data, large);
}

// ---------------------------------------------------------------------------
// Test 3: recv returns 0 when the peer performs a clean TLS shutdown
// ---------------------------------------------------------------------------
TEST_F(SSLSocketTest, CleanShutdownReturnsZero)
{
    ssize_t server_recv_result = -999;

    start_server(
        [&server_recv_result](SSLSocket ssl)
        {
            char buf[1024];
            server_recv_result = ssl.recv(buf, sizeof(buf));
        });

    {
        SSLSocket client = connect_client();
    }

    join_server();

    EXPECT_EQ(server_recv_result, 0);
}

// ---------------------------------------------------------------------------
// Test 4: send_with_timeout succeeds under normal conditions
// ---------------------------------------------------------------------------
TEST_F(SSLSocketTest, SendWithTimeoutSuccess)
{
    start_server(
        [](SSLSocket ssl)
        {
            char buf[4096] = {0};
            ssl.recv(buf, sizeof(buf));

            const char* reply = "OK";
            ssl.send(reply, strlen(reply));
        });

    SSLSocket client = connect_client();
    const char* msg = "timeout test";
    EXPECT_NO_THROW(client.send_with_timeout(msg, strlen(msg), 5000));

    char buf[64] = {0};
    client.recv(buf, sizeof(buf));

    join_server();

    EXPECT_STREQ(buf, "OK");
}

// ---------------------------------------------------------------------------
// Test 5: recv_with_timeout throws when no data arrives
// ---------------------------------------------------------------------------
TEST_F(SSLSocketTest, RecvWithTimeoutTimesOut)
{
    start_server(
        [](SSLSocket ssl)
        {
            std::this_thread::sleep_for(std::chrono::seconds(3));
        });

    SSLSocket client = connect_client();
    char buf[64];

    EXPECT_THROW(client.recv_with_timeout(buf, sizeof(buf), 200),
                 std::runtime_error);

    join_server();
}

// ---------------------------------------------------------------------------
// Test 6: recv_with_timeout succeeds when data arrives within the window
// ---------------------------------------------------------------------------
TEST_F(SSLSocketTest, RecvWithTimeoutSuccess)
{
    start_server(
        [](SSLSocket ssl)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const char* msg = "delayed hello";
            ssl.send(msg, strlen(msg));

            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        });

    SSLSocket client = connect_client();
    char buf[64] = {0};

    ssize_t n = client.recv_with_timeout(buf, sizeof(buf), 5000);

    join_server();

    EXPECT_GT(n, 0);
    EXPECT_STREQ(buf, "delayed hello");
}

// ---------------------------------------------------------------------------
// Test 7: A move-constructed SSLSocket is fully functional
// ---------------------------------------------------------------------------
TEST_F(SSLSocketTest, MoveConstructWorks)
{
    start_server(
        [](SSLSocket ssl)
        {
            char buf[4096] = {0};
            ssl.recv(buf, sizeof(buf));

            const char* reply = "moved";
            ssl.send(reply, strlen(reply));
        });

    SSLSocket original = connect_client();
    SSLSocket moved(std::move(original));

    const char* msg = "test move";
    moved.send(msg, strlen(msg));

    char buf[64] = {0};
    ssize_t n = moved.recv(buf, sizeof(buf));

    join_server();

    EXPECT_GT(n, 0);
    EXPECT_STREQ(buf, "moved");
}
