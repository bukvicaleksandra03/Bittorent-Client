#include "peer_wire/disk_writer.h"

#include <algorithm>
#include <fstream>
#include <memory>
#include <string>

DiskWriter::DiskWriter(const TorrentFile& torrent,
                       const std::string& output_dir)
    : m_output_dir(output_dir),
      m_piece_size(torrent.get_piece_size()),
      m_total_size(torrent.get_total_size())
{
    if (m_piece_size == 0)
    {
        LOG_AND_THROW("Invalid torrent piece size: 0");
    }
    if (m_total_size == 0)
    {
        LOG_AND_THROW("Invalid torrent total size: 0");
    }

    uint64_t running_offset = 0;
    for (const auto& layout_entry : torrent.file_layout())
    {
        FileEntry entry;
        entry.rel_path = layout_entry.first;
        entry.length = layout_entry.second;
        entry.offset = running_offset;
        running_offset += entry.length;
        m_files.push_back(std::move(entry));
    }

    if (m_files.empty())
    {
        LOG_AND_THROW("Torrent file layout is empty");
    }
    if (running_offset != m_total_size)
    {
        LOG_AND_THROW("Torrent file layout size mismatch: layout_total=" +
                      std::to_string(running_offset) +
                      ", torrent_total=" + std::to_string(m_total_size));
    }

    m_file_mutexes.reserve(m_files.size());
    for (size_t i = 0; i < m_files.size(); ++i)
    {
        m_file_mutexes.emplace_back(std::make_unique<std::mutex>());
    }
}

void DiskWriter::preallocate_files()
{
    std::filesystem::create_directories(m_output_dir);

    for (const auto& file : m_files)
    {
        const auto path = absolute_path(file);
        std::filesystem::create_directories(path.parent_path());

        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            LOG_AND_THROW("Failed to create output file: " + path.string());
        }

        if (file.length == 0)
        {
            continue;
        }

        out.seekp(static_cast<std::streamoff>(file.length - 1));
        const char zero = '\0';
        out.write(&zero, 1);
        if (!out)
        {
            LOG_AND_THROW("Failed to preallocate output file: " +
                          path.string());
        }
    }

    m_files_preallocated.store(true, std::memory_order_release);
}

void DiskWriter::ensure_preallocated() const
{
    if (!m_files_preallocated.load(std::memory_order_acquire))
    {
        LOG_AND_THROW(
            "DiskWriter: read_piece/write_piece called before "
            "preallocate_files() completed");
    }
}

void DiskWriter::write_piece(uint32_t index, const std::vector<uint8_t>& data)
{
    ensure_preallocated();
    std::vector<size_t> indices = file_indices_for_piece(index);
    std::sort(indices.begin(), indices.end());
    std::vector<std::unique_lock<std::mutex>> locks;
    locks.reserve(indices.size());
    for (size_t i : indices)
    {
        locks.emplace_back(*m_file_mutexes[i]);
    }

    const uint32_t expected_len = piece_length(index);
    if (data.size() != expected_len)
    {
        LOG_AND_THROW(
            "Piece write size mismatch: index=" + std::to_string(index) +
            ", expected=" + std::to_string(expected_len) +
            ", got=" + std::to_string(data.size()));
    }

    uint64_t write_offset = piece_global_offset(index);
    uint64_t remaining = data.size();
    size_t data_pos = 0;

    for (const auto& file : m_files)
    {
        if (remaining == 0)
        {
            break;
        }

        // Skip files that are entirely before the current write position
        if (write_offset >= file.offset + file.length)
        {
            continue;
        }

        // Stop the loop early if the next file starts after all remaining bytes
        if (write_offset + remaining <= file.offset)
        {
            break;
        }

        // How far into the file the write begins
        const uint64_t start_in_file =
            (write_offset > file.offset) ? (write_offset - file.offset) : 0;

        const uint64_t writable_in_file = file.length - start_in_file;
        const uint64_t chunk = std::min<uint64_t>(remaining, writable_in_file);

        const auto path = absolute_path(file);
        std::fstream io(path, std::ios::binary | std::ios::in | std::ios::out);
        if (!io)
        {
            LOG_AND_THROW("Failed to open output file for writing: " +
                          path.string());
        }

        io.seekp(static_cast<std::streamoff>(start_in_file));
        io.write(reinterpret_cast<const char*>(data.data() + data_pos),
                 static_cast<std::streamsize>(chunk));
        if (!io)
        {
            LOG_AND_THROW("Failed writing piece data to file: " +
                          path.string());
        }

        remaining -= chunk;
        data_pos += static_cast<size_t>(chunk);
        write_offset += chunk;
    }

    if (remaining != 0)
    {
        LOG_AND_THROW("Piece write did not fully map to files: index=" +
                      std::to_string(index) +
                      ", remaining=" + std::to_string(remaining));
    }
}

std::vector<uint8_t> DiskWriter::read_piece(uint32_t index)
{
    ensure_preallocated();
    std::vector<size_t> indices = file_indices_for_piece(index);
    std::sort(indices.begin(), indices.end());
    std::vector<std::unique_lock<std::mutex>> locks;
    locks.reserve(indices.size());
    for (size_t i : indices)
    {
        locks.emplace_back(*m_file_mutexes[i]);
    }

    const uint32_t len = piece_length(index);
    std::vector<uint8_t> out(len);

    uint64_t read_offset = piece_global_offset(index);
    uint64_t remaining = len;
    size_t out_pos = 0;

    for (const auto& file : m_files)
    {
        if (remaining == 0)
        {
            break;
        }

        if (read_offset >= file.offset + file.length)
        {
            continue;
        }
        if (read_offset + remaining <= file.offset)
        {
            break;
        }

        const uint64_t start_in_file =
            (read_offset > file.offset) ? (read_offset - file.offset) : 0;
        const uint64_t readable_in_file = file.length - start_in_file;
        const uint64_t chunk = std::min<uint64_t>(remaining, readable_in_file);

        const auto path = absolute_path(file);
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            LOG_AND_THROW("Failed to open output file for reading: " +
                          path.string());
        }

        in.seekg(static_cast<std::streamoff>(start_in_file));
        in.read(reinterpret_cast<char*>(out.data() + out_pos),
                static_cast<std::streamsize>(chunk));
        if (in.gcount() != static_cast<std::streamsize>(chunk))
        {
            LOG_AND_THROW("Failed reading piece data from file: " +
                          path.string());
        }

        remaining -= chunk;
        out_pos += static_cast<size_t>(chunk);
        read_offset += chunk;
    }

    if (remaining != 0)
    {
        LOG_AND_THROW("Piece read did not fully map to files: index=" +
                      std::to_string(index) +
                      ", remaining=" + std::to_string(remaining));
    }

    return out;
}

uint32_t DiskWriter::piece_length(uint32_t index) const
{
    const uint64_t begin = piece_global_offset(index);
    if (begin >= m_total_size)
    {
        LOG_AND_THROW("Piece index out of range for disk I/O: " +
                      std::to_string(index));
    }

    const uint64_t end = std::min<uint64_t>(begin + m_piece_size, m_total_size);
    return static_cast<uint32_t>(end - begin);
}

uint64_t DiskWriter::piece_global_offset(uint32_t index) const
{
    return static_cast<uint64_t>(index) * m_piece_size;
}

std::filesystem::path DiskWriter::absolute_path(const FileEntry& file) const
{
    return m_output_dir / file.rel_path;
}

std::vector<size_t> DiskWriter::file_indices_for_piece(uint32_t index) const
{
    const uint64_t beg = piece_global_offset(index);
    const uint64_t end = beg + piece_length(index);
    std::vector<size_t> out;
    out.reserve(2);

    for (size_t i = 0; i < m_files.size(); ++i)
    {
        const auto& f = m_files[i];
        const auto file_start = f.offset;
        const uint64_t file_end = file_start + f.length;
        if (end <= file_start || beg >= file_end)
        {
            continue;
        }
        out.push_back(i);
    }
    return out;
}
