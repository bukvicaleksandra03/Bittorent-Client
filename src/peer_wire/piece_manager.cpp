#include "peer_wire/piece_manager.h"

#include <stdexcept>
#include <string>

#include "logger.h"
#include "peer_wire/disk_writer.h"

PieceManager::PieceManager(uint32_t num_pieces,
                           uint32_t piece_length,
                           uint64_t total_length,
                           std::vector<crypto::SHA1Hash> piece_hashes,
                           DiskWriter& disk_writer)
    : m_total_length_bytes(total_length),
      m_num_pieces(num_pieces),
      m_nominal_piece_length(piece_length),
      m_piece_hashes(std::move(piece_hashes)),
      m_have(std::make_unique<std::atomic<bool>[]>(num_pieces)),
      m_claimed(std::make_unique<std::atomic<bool>[]>(num_pieces)),
      m_disk_writer(disk_writer)
{
    if (m_piece_hashes.size() != num_pieces)
    {
        throw std::runtime_error("piece hash count does not match num_pieces");
    }
    // make_unique<std::atomic<bool>[]> value-initialises each element to
    // false (zero-initialisation of the atomic's stored value).
}

// ---------------------------------------------------------------------------
// next_needed — fully lock-free
//
// Iterates pieces and tries to atomically claim each one via CAS.
// Multiple peer threads can call this simultaneously; the CAS guarantees
// exactly one thread wins each piece.
// ---------------------------------------------------------------------------
std::optional<uint32_t> PieceManager::next_needed(
    const std::vector<bool>& peer_bitfield)
{
    for (uint32_t i = 0; i < m_num_pieces; ++i)
    {
        // Skip pieces the peer doesn't have.
        if (i >= static_cast<uint32_t>(peer_bitfield.size()) ||
            !peer_bitfield[i])
        {
            continue;
        }

        // Skip pieces we already finished.
        if (m_have[i].load(std::memory_order_acquire))
        {
            continue;
        }

        // Try to atomically claim this piece.  Only one thread will succeed
        // even when many threads race here simultaneously.
        bool expected = false;
        if (m_claimed[i].compare_exchange_strong(expected,
                                                 true,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_relaxed))
        {
            // We own piece i.  The caller will allocate the buffer locally.
            return i;
        }
        // Another thread claimed it; keep searching.
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
        m_total_length_bytes -
        static_cast<uint64_t>(m_nominal_piece_length) * (m_num_pieces - 1);
    return static_cast<uint32_t>(remainder);
}

uint32_t PieceManager::num_pieces() const
{
    return m_num_pieces;
}

// ---------------------------------------------------------------------------
// complete_piece — called by the owning peer thread
//
// The data buffer is fully assembled in the caller's InFlightPiece; this
// function only does hash verification, disk scheduling, and atomic state
// updates.
// ---------------------------------------------------------------------------
bool PieceManager::complete_piece(uint32_t piece_index,
                                  const std::vector<uint8_t>& data)
{
    if (piece_index >= m_num_pieces)
    {
        throw std::runtime_error(
            "out-of-range piece index: " + std::to_string(piece_index) +
            ", num_pieces=" + std::to_string(m_num_pieces));
    }

    // SHA-1 verification happens with no shared state touched.
    crypto::SHA1Hash actual = crypto::sha1(data);
    if (actual != m_piece_hashes[piece_index])
    {
        if (m_logger)
            LLOG_WARNING(
                *m_logger,
                "piece " + std::to_string(piece_index) +
                    " failed SHA-1 verification; releasing claim for retry");

        // Release the claim so another peer can re-download this piece.
        m_claimed[piece_index].store(false, std::memory_order_release);
        return false;
    }

    // Write to disk before marking the piece as available.
    m_disk_writer.write_piece(piece_index, data);

    // Mark the piece permanently done.  Leave m_claimed[piece_index] = true
    // so next_needed never tries to re-claim an already-complete piece (even
    // in the brief window before m_have is visible).
    m_have[piece_index].store(true, std::memory_order_release);
    m_curr_downloaded_bytes.fetch_add(piece_length(piece_index),
                                      std::memory_order_relaxed);
    m_completed_count.fetch_add(1, std::memory_order_acq_rel);

    return true;
}

bool PieceManager::is_complete() const
{
    return m_completed_count.load(std::memory_order_acquire) == m_num_pieces;
}

bool PieceManager::have_piece(uint32_t index) const
{
    if (index >= m_num_pieces)
        return false;
    return m_have[index].load(std::memory_order_acquire);
}

void PieceManager::abort_piece(uint32_t index)
{
    if (index >= m_num_pieces)
    {
        // The index comes from InFlightPiece::piece_index, which is always
        // assigned by next_needed — this should never be out of range.
        if (m_logger)
            LLOG_WARNING(*m_logger,
                         "abort_piece called with out-of-range index=" +
                             std::to_string(index) +
                             ", num_pieces=" + std::to_string(m_num_pieces));
        return;
    }

    // Only release if the piece isn't already verified.  A peer that
    // received all blocks and successfully called complete_piece should
    // never be calling abort_piece, but guard against it anyway.
    if (!m_have[index].load(std::memory_order_acquire))
    {
        m_claimed[index].store(false, std::memory_order_release);
    }
}
