#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
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

static void wait_for_server(std::atomic<bool>& ready)
{
    while (!ready)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
}

static TCPServerSocket make_listening_server(int port)
{
    IPv4Address addr(INADDR_ANY, port);
    TCPServerSocket server(AF_INET);
    int opt = 1;
    setsockopt(server.get_fd(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    server.bind(addr);
    server.listen(1);
    return server;
}

static SSLSocket make_test_client(int port)
{
    IPv4Address addr("127.0.0.1", port);
    TCPClientSocket tcp(AF_INET);
    tcp.connect(addr);
    return SSLSocket(std::move(tcp), "localhost");
}

// ---------------------------------------------------------------------------
// Test 1: Client sends a message, server echoes back a response
// ---------------------------------------------------------------------------
TEST(SSLSocketTest, BasicSendReceive)
{
    const int PORT = 19101;
    std::atomic<bool> ready{false};
    std::string error_msg;

    std::thread server_thread([&]() {
        try
        {
            TCPServerSocket server = make_listening_server(PORT);
            ready = true;

            TCPAcceptSocket accepted = server.accept();
            SSLSocket ssl_server(std::move(accepted), CERT_FILE, KEY_FILE);

            char buf[4096] = {0};
            ssl_server.recv(buf, sizeof(buf));

            const char* reply = "Hello from TLS server";
            ssl_server.send(reply, strlen(reply));
        }
        catch (const std::exception& e)
        {
            error_msg = e.what();
        }
    });

    wait_for_server(ready);

    SSLSocket client = make_test_client(PORT);
    const char* msg = "Hello from TLS client";
    client.send(msg, strlen(msg));

    char buf[4096] = {0};
    ssize_t n = client.recv(buf, sizeof(buf));

    server_thread.join();

    ASSERT_TRUE(error_msg.empty()) << "Server error: " << error_msg;
    EXPECT_GT(n, 0);
    EXPECT_STREQ(buf, "Hello from TLS server");
}

// ---------------------------------------------------------------------------
// Test 2: Send a large payload (100 KB) to exercise the send loop
// ---------------------------------------------------------------------------
TEST(SSLSocketTest, LargePayload)
{
    const int PORT = 19102;
    std::atomic<bool> ready{false};
    std::string received_data;
    std::string error_msg;

    std::thread server_thread([&]() {
        try
        {
            TCPServerSocket server = make_listening_server(PORT);
            ready = true;

            TCPAcceptSocket accepted = server.accept();
            SSLSocket ssl_server(std::move(accepted), CERT_FILE, KEY_FILE);

            received_data = ssl_server.recv_all();
        }
        catch (const std::exception& e)
        {
            error_msg = e.what();
        }
    });

    wait_for_server(ready);

    const size_t PAYLOAD_SIZE = 100 * 1024;
    std::string large(PAYLOAD_SIZE, 'A');

    {
        SSLSocket client = make_test_client(PORT);
        client.send(large.c_str(), large.size());
    }

    server_thread.join();

    ASSERT_TRUE(error_msg.empty()) << "Server error: " << error_msg;
    EXPECT_EQ(received_data.size(), PAYLOAD_SIZE);
    EXPECT_EQ(received_data, large);
}

// ---------------------------------------------------------------------------
// Test 3: recv returns 0 when the peer performs a clean TLS shutdown
// ---------------------------------------------------------------------------
TEST(SSLSocketTest, CleanShutdownReturnsZero)
{
    const int PORT = 19103;
    std::atomic<bool> ready{false};
    ssize_t server_recv_result = -999;
    std::string error_msg;

    std::thread server_thread([&]() {
        try
        {
            TCPServerSocket server = make_listening_server(PORT);
            ready = true;

            TCPAcceptSocket accepted = server.accept();
            SSLSocket ssl_server(std::move(accepted), CERT_FILE, KEY_FILE);

            char buf[1024];
            server_recv_result = ssl_server.recv(buf, sizeof(buf));
        }
        catch (const std::exception& e)
        {
            error_msg = e.what();
        }
    });

    wait_for_server(ready);

    {
        SSLSocket client = make_test_client(PORT);
    }

    server_thread.join();

    ASSERT_TRUE(error_msg.empty()) << "Server error: " << error_msg;
    EXPECT_EQ(server_recv_result, 0);
}

// ---------------------------------------------------------------------------
// Test 4: send_with_timeout succeeds under normal conditions
// ---------------------------------------------------------------------------
TEST(SSLSocketTest, SendWithTimeoutSuccess)
{
    const int PORT = 19104;
    std::atomic<bool> ready{false};
    std::string error_msg;

    std::thread server_thread([&]() {
        try
        {
            TCPServerSocket server = make_listening_server(PORT);
            ready = true;

            TCPAcceptSocket accepted = server.accept();
            SSLSocket ssl_server(std::move(accepted), CERT_FILE, KEY_FILE);

            char buf[4096] = {0};
            ssl_server.recv(buf, sizeof(buf));

            const char* reply = "OK";
            ssl_server.send(reply, strlen(reply));
        }
        catch (const std::exception& e)
        {
            error_msg = e.what();
        }
    });

    wait_for_server(ready);

    SSLSocket client = make_test_client(PORT);
    const char* msg = "timeout test";
    EXPECT_NO_THROW(client.send_with_timeout(msg, strlen(msg), 5000));

    char buf[64] = {0};
    client.recv(buf, sizeof(buf));

    server_thread.join();

    ASSERT_TRUE(error_msg.empty()) << "Server error: " << error_msg;
    EXPECT_STREQ(buf, "OK");
}

// ---------------------------------------------------------------------------
// Test 5: recv_with_timeout throws when no data arrives
// ---------------------------------------------------------------------------
TEST(SSLSocketTest, RecvWithTimeoutTimesOut)
{
    const int PORT = 19105;
    std::atomic<bool> ready{false};
    std::string error_msg;

    std::thread server_thread([&]() {
        try
        {
            TCPServerSocket server = make_listening_server(PORT);
            ready = true;

            TCPAcceptSocket accepted = server.accept();
            SSLSocket ssl_server(std::move(accepted), CERT_FILE, KEY_FILE);

            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
        catch (const std::exception& e)
        {
            error_msg = e.what();
        }
    });

    wait_for_server(ready);

    SSLSocket client = make_test_client(PORT);
    char buf[64];

    EXPECT_THROW(client.recv_with_timeout(buf, sizeof(buf), 200),
                 std::runtime_error);

    server_thread.join();
}

// ---------------------------------------------------------------------------
// Test 6: recv_with_timeout succeeds when data arrives within the window
// ---------------------------------------------------------------------------
TEST(SSLSocketTest, RecvWithTimeoutSuccess)
{
    const int PORT = 19106;
    std::atomic<bool> ready{false};
    std::string error_msg;

    std::thread server_thread([&]() {
        try
        {
            TCPServerSocket server = make_listening_server(PORT);
            ready = true;

            TCPAcceptSocket accepted = server.accept();
            SSLSocket ssl_server(std::move(accepted), CERT_FILE, KEY_FILE);

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const char* msg = "delayed hello";
            ssl_server.send(msg, strlen(msg));

            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        catch (const std::exception& e)
        {
            error_msg = e.what();
        }
    });

    wait_for_server(ready);

    SSLSocket client = make_test_client(PORT);
    char buf[64] = {0};

    ssize_t n = client.recv_with_timeout(buf, sizeof(buf), 5000);

    server_thread.join();

    ASSERT_TRUE(error_msg.empty()) << "Server error: " << error_msg;
    EXPECT_GT(n, 0);
    EXPECT_STREQ(buf, "delayed hello");
}

// ---------------------------------------------------------------------------
// Test 7: A move-constructed SSLSocket is fully functional
// ---------------------------------------------------------------------------
TEST(SSLSocketTest, MoveConstructWorks)
{
    const int PORT = 19107;
    std::atomic<bool> ready{false};
    std::string error_msg;

    std::thread server_thread([&]() {
        try
        {
            TCPServerSocket server = make_listening_server(PORT);
            ready = true;

            TCPAcceptSocket accepted = server.accept();
            SSLSocket ssl_server(std::move(accepted), CERT_FILE, KEY_FILE);

            char buf[4096] = {0};
            ssl_server.recv(buf, sizeof(buf));

            const char* reply = "moved";
            ssl_server.send(reply, strlen(reply));
        }
        catch (const std::exception& e)
        {
            error_msg = e.what();
        }
    });

    wait_for_server(ready);

    SSLSocket original = make_test_client(PORT);
    SSLSocket moved(std::move(original));

    const char* msg = "test move";
    moved.send(msg, strlen(msg));

    char buf[64] = {0};
    ssize_t n = moved.recv(buf, sizeof(buf));

    server_thread.join();

    ASSERT_TRUE(error_msg.empty()) << "Server error: " << error_msg;
    EXPECT_GT(n, 0);
    EXPECT_STREQ(buf, "moved");
}
