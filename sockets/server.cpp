#include <iostream>
#include "socket_addresses.h"
#include "socket.h"

#define PORT 8080
#define BUFFER_SIZE 1024

int main() {
    char buffer[BUFFER_SIZE] = {0};
    const char *response = "Hello from server";

    IPv4Address anyAddr(INADDR_ANY, PORT);

    Socket server_socket(AF_INET);

    server_socket.bind(anyAddr);
    server_socket.listen(3);

    std::cout << "Server listening on port " << PORT << std::endl;

    Socket accept_socket = server_socket.accept();

    std::cout << "Client connected from: " << accept_socket.get_address().identifier << std::endl;

    accept_socket.read(buffer, BUFFER_SIZE);
    std::cout << "Message from client: " << buffer << std::endl;

    accept_socket.send(response, strlen(response));
    std::cout << "Response sent" << std::endl;

    return 0;
}
