#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
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

    // One mutex per output file so concurrent pieces touching disjoint files
    // do not serialize. Spanning pieces lock two mutexes in ascending index
    // order to avoid deadlock.
    std::vector<std::unique_ptr<std::mutex>> m_file_mutexes;

    // Set with release semantics at the end of preallocate_files(); read/write
    // use acquire to ensure file creation is visible before I/O.
    std::atomic<bool> m_files_preallocated{false};

    void ensure_preallocated() const;

    uint32_t piece_length(uint32_t index) const;
    uint64_t piece_global_offset(uint32_t index) const;
    std::filesystem::path absolute_path(const FileEntry& file) const;

    // File indices whose byte range overlaps this piece (sorted ascending).
    std::vector<size_t> file_indices_for_piece(uint32_t index) const;
};
