#include <iostream>
#include <memory>
#include <filesystem>
#include <fstream>
namespace fs = std::filesystem;

#include "bencode_types.h"
#include "bencode_parser.h"
#include "torrent_file.h"

int main() {
    try {
        std::string output_dir = "/home/aleksandra/Desktop/BittorentClient/parsed_torrents";
        for (const auto &file : fs::directory_iterator("/home/aleksandra/Desktop/BittorentClient/torrents"))
        {
            if (!fs::is_regular_file(file.status())) continue;
            std::shared_ptr<BDict> metadata_dict = std::make_shared<BDict>();
            BencodeParser parser(file.path(), metadata_dict);
            parser.parse();
            TorrentFile torrent_file(metadata_dict);

            fs::path output_file = fs::path(output_dir) / file.path().filename();
            std::ofstream out_file(output_file);
            parser.print(out_file);
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
 
    return 0;
}
