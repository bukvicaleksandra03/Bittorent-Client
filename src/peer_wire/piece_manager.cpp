#include "peer_wire/piece_manager.h"

#include <algorithm>
#include <cstring>
#include <string>

#include "logger.h"
#include "peer_wire/disk_writer.h"

PieceManager::PieceManager(uint32_t num_pieces,
                           uint32_t piece_length,
                           uint64_t total_length,
                           std::vector<crypto::SHA1Hash> piece_hashes,
                           DiskWriter& disk_writer)
    : m_total_length(total_length),
      m_curr_downloaded(0),
      m_num_pieces(num_pieces),
      m_nominal_piece_length(piece_length),
      m_piece_hashes(std::move(piece_hashes)),
      m_have(num_pieces, false),
      m_in_progress(num_pieces, false),
      m_buffers(num_pieces),
      m_disk_writer(disk_writer)
{
    if (m_piece_hashes.size() != num_pieces)
    {
        LOG_AND_THROW("piece hash count does not match num_pieces");
    }
}

std::optional<uint32_t> PieceManager::next_needed(
    const std::vector<bool>& peer_bitfield)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (uint32_t i = 0; i < m_num_pieces; ++i)
    {
        if (!m_have[i] && !m_in_progress[i] && i < peer_bitfield.size() &&
            peer_bitfield[i])
        {
            m_in_progress[i] = true;

            uint32_t plen = piece_length(i);
            uint32_t num_blocks = (plen + BLOCK_SIZE - 1) / BLOCK_SIZE;

            auto& buf = m_buffers[i];
            buf.data.resize(plen, 0);
            buf.blocks_received.assign(num_blocks, false);
            buf.blocks_done = 0;
            buf.blocks_total = num_blocks;

            return i;
        }
    }
    return std::nullopt;
}

uint32_t PieceManager::piece_length(uint32_t index) const
{
    if (index + 1 < m_num_pieces)
    {
        return m_nominal_piece_length;
    }
    uint64_t remainder =
        m_total_length -
        static_cast<uint64_t>(m_nominal_piece_length) * (m_num_pieces - 1);
    return static_cast<uint32_t>(remainder);
}

uint32_t PieceManager::num_pieces() const
{
    return m_num_pieces;
}

bool PieceManager::receive_block(uint32_t piece_index,
                                 uint32_t begin,
                                 const uint8_t* data,
                                 size_t len)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (piece_index >= m_num_pieces)
    {
        LOG_AND_THROW(
            "out-of-range piece index: " + std::to_string(piece_index) +
            ", num_pieces=" + std::to_string(m_num_pieces));
    }

    if (!m_in_progress[piece_index])
    {
        LOG_D("Ignoring stale block for piece " + std::to_string(piece_index) +
              ": piece is not currently in progress");
        return false;
    }

    auto& buf = m_buffers[piece_index];

    if (begin + len > buf.data.size())
    {
        LOG_D("Ignoring out-of-bounds block for piece " +
              std::to_string(piece_index) + ": begin=" + std::to_string(begin) +
              ", len=" + std::to_string(len) +
              ", piece_size=" + std::to_string(buf.data.size()));
        return false;
    }

    uint32_t block_idx = begin / BLOCK_SIZE;
    if (block_idx >= buf.blocks_received.size() ||
        buf.blocks_received[block_idx])
    {
        LOG_D("Ignoring invalid/duplicate block for piece " +
              std::to_string(piece_index) +
              ": block_idx=" + std::to_string(block_idx) +
              ", blocks_total=" + std::to_string(buf.blocks_received.size()) +
              ", already_received=" +
              std::string(block_idx < buf.blocks_received.size() &&
                                  buf.blocks_received[block_idx]
                              ? "true"
                              : "false"));
        return false;
    }

    std::memcpy(buf.data.data() + begin, data, len);
    buf.blocks_received[block_idx] = true;
    ++buf.blocks_done;

    if (buf.blocks_done < buf.blocks_total)
    {
        // still haven't received the full piece yet
        return false;
    }

    if (verify_piece(piece_index))
    {
        // verification passed
        m_disk_writer.write_piece(piece_index, buf.data);
        m_have[piece_index] = true;
        m_in_progress[piece_index] = false;
        m_curr_downloaded += piece_length(piece_index);
        buf = {};
        return true;
    }

    // verification failed, the piece downloading can be restarted
    m_in_progress[piece_index] = false;
    buf = {};
    return false;
}

bool PieceManager::is_complete() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return std::all_of(m_have.begin(), m_have.end(), [](bool v) { return v; });
}

bool PieceManager::have_piece(uint32_t index) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index >= m_num_pieces)
        return false;
    return m_have[index];
}

void PieceManager::abort_piece(uint32_t index)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index < m_num_pieces && m_in_progress[index])
    {
        m_in_progress[index] = false;
        m_buffers[index] = {};
    }
}

bool PieceManager::verify_piece(uint32_t index) const
{
    const auto& buf = m_buffers[index];
    crypto::SHA1Hash actual = crypto::sha1(buf.data);
    return actual == m_piece_hashes[index];
}
