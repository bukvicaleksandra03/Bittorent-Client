#ifndef __TORRENT_FILE_H_
#define __TORRENT_FILE_H_

#include <filesystem>
#include <cstdint>
#include <cmath>
#include <array>
#include <algorithm>
#include "bencode_parser.h"
namespace fs = std::filesystem;

class TorrentFile
{
public:
    explicit TorrentFile(std::shared_ptr<BDict> metadata_dict);

    struct File {
        std::string crc32;
        uint64_t length;
        std::string md5;
        std::string mtime;
        fs::path path;
        std::string sha1;
    };

private:
    std::string announce;
    std::vector<std::vector<std::string>> announce_list;
    std::string torrent_name;

    bool is_multifile;
    
    uint64_t total_size;
    uint64_t piece_size;
    std::vector<File> files;
    bool is_private;

    std::vector<std::array<uint8_t, 20>> pieces;
    std::string info_hash;
    
    std::vector<std::string> load_list_of_bstrings(BList* list);
};

#endif // __TORRENT_FILE_H_