#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>

#include "bencode_types.h"
#include "crypto.h"
#include "tracker_protocol.h"
namespace fs = std::filesystem;

class TorrentFile
{
   public:
    // Constructor takes metadata dict and raw info bytes for hash calculation
    TorrentFile(std::shared_ptr<BDict> metadata_dict,
                const std::vector<uint8_t>& info_dict_raw_bytes);

    void print(std::ostream& os);

    // Getters
    const std::string& get_announce() const
    {
        return announce;
    }
    const std::string& get_name() const
    {
        return torrent_name;
    }
    uint64_t get_total_size() const
    {
        return total_size;
    }
    uint64_t get_piece_size() const
    {
        return piece_size;
    }
    size_t get_piece_count() const
    {
        return pieces.size();
    }
    const crypto::SHA1Hash& get_info_hash() const
    {
        return info_hash;
    }
    const std::vector<crypto::SHA1Hash>& get_piece_hashes() const
    {
        return pieces;
    }
    std::string get_info_hash_hex() const
    {
        return crypto::to_hex(info_hash);
    }

    const std::string& get_tracker_hostname() const
    {
        return tracker_hostname;
    }
    uint16_t get_tracker_port() const
    {
        return tracker_port;
    }
    const std::string& get_tracker_path() const
    {
        return tracker_path;
    }
    TrackerProtocol get_tracker_protocol() const
    {
        return tracker_protocol;
    }

   private:
    struct File
    {
        std::string crc32;
        uint64_t length;
        std::string md5;
        std::string mtime;
        fs::path path;
        std::string sha1;
    };

    std::string announce;
    std::vector<std::vector<std::string>> announce_list;
    std::string torrent_name;

    bool is_multifile;

    uint64_t total_size;
    uint64_t piece_size;
    std::vector<File> files;
    bool is_private = false;

    std::vector<std::array<uint8_t, 20>> pieces;
    crypto::SHA1Hash info_hash;

    // Tracker information
    std::string tracker_hostname;
    uint16_t tracker_port;
    std::string tracker_path;
    TrackerProtocol tracker_protocol;

    void extract_tracker_information(const std::string& announce);

    std::vector<std::string> load_list_of_bstrings(BList* list);
};