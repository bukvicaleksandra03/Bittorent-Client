#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <utility>
#include <vector>

#include "bencode/bencode_types.h"
#include "crypto.h"
#include "info_hash.h"
#include "trackers/tracker_details.h"
namespace fs = std::filesystem;

class TorrentFile
{
   public:
    // Builds a TorrentFile from the parsed bencoded dictionary.
    // info_dict_raw_bytes is the exact byte sequence of the bencoded "info"
    // value, needed to compute info_hash (SHA-1 must hash the raw bytes,
    // not a re-encoded representation).
    TorrentFile(std::shared_ptr<BDict> metadata_dict,
                const std::vector<uint8_t>& info_dict_raw_bytes);

    void print(std::ostream& os);

    // Getters
    std::string get_announce() const
    {
        return announce_tracker.to_string();
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
    bool is_multi_file() const
    {
        return is_multifile;
    }
    const InfoHash& get_info_hash() const
    {
        return info_hash;
    }
    const std::vector<crypto::SHA1Hash>& get_piece_hashes() const
    {
        return pieces;
    }
    std::string get_info_hash_hex() const
    {
        return info_hash.hex();
    }

    TrackerDetails get_tracker() const
    {
        return announce_tracker;
    }

    const std::vector<std::vector<TrackerDetails>>& get_announce_list() const
    {
        return announce_list_trackers;
    }

    // Returns the logical output file layout as (relative_path, length) pairs.
    // For single-file torrents this contains exactly one entry.
    std::vector<std::pair<fs::path, uint64_t>> file_layout() const
    {
        std::vector<std::pair<fs::path, uint64_t>> layout;
        if (is_multifile)
        {
            layout.reserve(files.size());
            for (const auto& f : files)
            {
                layout.emplace_back(f.path, f.length);
            }
        }
        else
        {
            layout.emplace_back(fs::path(torrent_name), total_size);
        }
        return layout;
    }

   private:
    // Represents a single file within the torrent
    struct File
    {
        // Mandatory
        uint64_t length;  // File size in bytes (mandatory)
        fs::path path;    // Relative path where the file should be saved

        // Optional
        std::string mtime;  // Last-modification Unix timestamp
        std::string sha1;   // SHA-1 hash for per-file integrity

        // Practically useless because SHA1 already detects everything CRC-32
        // and MD5 detect (accidental corruption, truncation, bit flips). SHA-1
        // is strictly stronger than both -- larger output space (160 bits vs 32
        // or 128) and better collision resistance. Internet archive still adds
        // them for legacy/compatibility reasons.
        std::string crc32;  // CRC-32 checksum
        std::string md5;    // Optional MD5 hash for per-file integrity (BEP 3)
    };

    std::string torrent_name;  // Suggested name for the file or root directory

    bool is_multifile;  // True if torrent contains multiple files

    uint64_t total_size;      // Total size of all files combined, in bytes
    uint64_t piece_size;      // Size of each piece (except possibly the last)
    std::vector<File> files;  // List of files in the torrent
    bool is_private =
        false;  // If true, clients must only use trackers (no DHT/PEX)

    // SHA-1 of the raw bencoded info dict (torrent identity)
    InfoHash info_hash;
    std::vector<crypto::SHA1Hash> pieces;  // SHA-1 hash for each piece

    // created when details from "announce" are extracted
    TrackerDetails announce_tracker;
    // created when details from "announce list" are extracted
    std::vector<std::vector<TrackerDetails>> announce_list_trackers;

    // Parses the announce URL into hostname, port, path, and protocol
    TrackerDetails extract_tracker_information(const std::string& announce);

    // Utility: extracts a BList of BStrings into a vector of std::string
    std::vector<std::string> load_list_of_bstrings(BList* list);
};