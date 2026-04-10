#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

#include "crypto.h"

inline constexpr uint32_t BLOCK_SIZE = 1 << 14;  // 16 KiB

class PieceManager
{
   public:
    PieceManager(uint32_t num_pieces, uint32_t piece_length,
                 uint64_t total_length,
                 std::vector<crypto::SHA1Hash> piece_hashes);

    // Pick a piece index this peer has that we still need.
    // Returns std::nullopt when nothing useful remains.
    std::optional<uint32_t> next_needed(
        const std::vector<bool>& peer_bitfield);

    // Byte length of a given piece (last piece may be shorter).
    uint32_t piece_length(uint32_t index) const;

    uint32_t num_pieces() const;

    // Store a received block.  Returns true when the full piece is
    // complete and its SHA-1 matches the expected hash.
    bool receive_block(uint32_t piece_index, uint32_t begin,
                       const uint8_t* data, size_t len);

    bool is_complete() const;

    bool have_piece(uint32_t index) const;

    // Mark a piece as no longer in progress (e.g. peer disconnected).
    void abort_piece(uint32_t index);

   private:
    uint32_t m_num_pieces;
    uint32_t m_nominal_piece_length;
    uint64_t m_total_length;
    std::vector<crypto::SHA1Hash> m_piece_hashes;

    std::vector<bool> m_have;
    std::vector<bool> m_in_progress;

    struct PieceBuffer
    {
        std::vector<uint8_t> data;
        std::vector<bool> blocks_received;
        uint32_t blocks_done = 0;
        uint32_t blocks_total = 0;
    };
    std::vector<PieceBuffer> m_buffers;

    mutable std::mutex m_mutex;

    bool verify_piece(uint32_t index) const;
};
