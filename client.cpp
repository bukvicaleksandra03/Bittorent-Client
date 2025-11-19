#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include "inc/socket_addresses.h"
#include "socket.h"

#define PORT 8080
#define BUFFER_SIZE 1024

int main() {
    char buffer[BUFFER_SIZE] = {0};
    const char *message = "Hello from client";

    IPv4Address address("127.0.0.1", PORT);
    Socket client_socket(AF_INET);
    client_socket.connect(address);


    client_socket.send(message, strlen(message));
    std::cout << "Message sent to server" << std::endl;

    client_socket.read(buffer, BUFFER_SIZE);
    std::cout << "Response from server: " << buffer << std::endl;

    return 0;
}