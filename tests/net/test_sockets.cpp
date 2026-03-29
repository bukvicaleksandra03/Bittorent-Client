#include <gtest/gtest.h>

#include <cstring>
#include <iostream>
#include <thread>

#include "net/socket.h"
#include "net/socket_addresses.h"

#define BUFFER_SIZE 1024
#define PORT 9090

// Simple server function
void run_server(bool &ready_flag)
{
    char buffer[BUFFER_SIZE] = {0};
    const char *response = "Hello from server";

    // The machine has multiple IP addresses 127.0.0.1 (loopback)
    // 192.168.1.5 (Wi-Fi), maybe 10.x.x.x (VPN, Docker, etc.).
    // This marks that we want to accept connections sent to any of these
    // IPs on port PORT.
    IPv4Address anyAddr(INADDR_ANY, PORT);

    TCPServerSocket server_socket(AF_INET);

    server_socket.bind(anyAddr);
    server_socket.listen(3);

    ready_flag = true;  // signal server is ready

    std::cout << "Server listening on port " << PORT << std::endl;

    TCPAcceptSocket accept_socket = server_socket.accept();

    accept_socket.recv(buffer, BUFFER_SIZE);
    std::cout << "Message from client: " << buffer << std::endl;

    accept_socket.send(response, strlen(response));
    std::cout << "Response sent" << std::endl;
}

// Test case
TEST(SocketIntegrationTest, SendReceive)
{
    bool server_ready = false;

    std::thread server_thread(run_server, std::ref(server_ready));

    // Wait until server is ready (simple sync)
    while (!server_ready)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // --- Client ---
    char buffer[BUFFER_SIZE] = {0};
    const char *message = "Hello from client";

    IPv4Address address("127.0.0.1", PORT);
    TCPClientSocket client_socket(AF_INET, SOCK_STREAM);
    client_socket.connect(address);

    client_socket.send(message, strlen(message));
    std::cout << "Message sent to server" << std::endl;

    client_socket.recv(buffer, BUFFER_SIZE);
    std::cout << "Response from server: " << buffer << std::endl;

    server_thread.join();
}