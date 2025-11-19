#include "bencode_parser.h"
#include <iostream>

int main() {
    try {
        BencodeParser parser("/home/aleksandra/Desktop/BittorentClient/ubuntu-25.10-desktop-amd64.iso.torrent");
        parser.parse();

        std::cout << "Parsed Bencode dictionary:\n";
        parser.print();
        std::cout << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }

    return 0;
}
