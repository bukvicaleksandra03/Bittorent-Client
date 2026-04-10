#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "torrent_file.h"

class DiskWriter
{
   public:
    DiskWriter(const TorrentFile& torrent, const std::string& output_dir);

    // Creates output files with the expected final lengths.
    void preallocate_files();

    // Writes one fully verified piece to disk.
    void write_piece(uint32_t index, const std::vector<uint8_t>& data);

    // Reads one piece back from disk (used by future seeding path).
    std::vector<uint8_t> read_piece(uint32_t index);

   private:
    struct FileEntry
    {
        std::filesystem::path rel_path;
        uint64_t length = 0;
        uint64_t offset = 0;  // global torrent byte offset
    };

    std::filesystem::path m_output_dir;
    uint64_t m_piece_size = 0;
    uint64_t m_total_size = 0;
    std::vector<FileEntry> m_files;
    mutable std::mutex m_mutex;

    uint32_t piece_length(uint32_t index) const;
    uint64_t piece_global_offset(uint32_t index) const;
    std::filesystem::path absolute_path(const FileEntry& file) const;
};
