#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "crypto.h"
#include "logger.h"

class DiskWriter;

inline constexpr uint32_t BLOCK_SIZE = 1 << 14;  // 16 KiB

// Result of next_needed(): which piece to download and whether this peer
// exclusively claimed it (must abort_piece on failure/disconnect).
struct NeededPiece
{
    uint32_t index = 0;
    bool exclusive = false;
};

// ---------------------------------------------------------------------------
// PieceManager — lock-free coordination layer
//
// Design contract
// ---------------
// Each piece is owned by exactly ONE peer thread at a time.
//
//   next_needed()   — atomically claims a piece (CAS on m_claimed).
//                     No mutex; multiple threads may call concurrently.
//
//   complete_piece() — called by the owning thread when all blocks for the
//                      piece have been assembled in its local buffer.  Runs
//                      SHA-1 verification, schedules the disk write, and
//                      atomically marks the piece done.  No mutex.
//
//   abort_piece()   — releases a claim so the piece can be retried by any
//                     other peer.  Atomic store; no mutex.
//
//   is_complete()   — single atomic counter load; no mutex.
//   have_piece()    — single atomic load; no mutex.
//
// The block-receive hot path (writing raw bytes) lives entirely in
// PeerConnection::InFlightPiece, which is thread-local state. PieceManager
// never touches the per-block data buffer.
// ---------------------------------------------------------------------------
class PieceManager
{
   public:
    PieceManager(uint32_t num_pieces,
                 uint32_t piece_length,
                 uint64_t total_length,
                 std::vector<crypto::SHA1Hash> piece_hashes,
                 DiskWriter& disk_writer);

    // Pick the next piece this peer has that we still need.
    // Normally atomically claims the piece via CAS (exclusive=true).
    // When every remaining piece is already claimed, enters endgame mode and
    // returns an incomplete piece without claiming (exclusive=false) so
    // multiple peers can request the same last piece(s).
    std::optional<NeededPiece> next_needed(
        const std::vector<bool>& peer_bitfield);

    // Byte length of a given piece (last piece may be shorter).
    uint32_t piece_length(uint32_t index) const;

    uint32_t num_pieces() const;

    // Called by the owning peer thread after assembling all blocks locally.
    // Verifies SHA-1, writes to disk, and marks the piece complete.
    // Returns true on success or if another peer already completed the piece
    // (endgame race). On hash failure returns false; the caller must call
    // abort_piece only when it holds an exclusive claim.
    bool complete_piece(uint32_t piece_index, const std::vector<uint8_t>& data);

    // True once every piece has been successfully completed.
    // Lock-free: single atomic counter comparison.
    bool is_complete() const;

    // Lock-free: single atomic load.
    bool have_piece(uint32_t index) const;

    // Snapshot of which pieces we currently hold (true = verified + on disk).
    // Used to advertise our Bitfield to peers when seeding. Lock-free.
    std::vector<bool> current_bitfield() const;

    // Read the block [begin, begin + length) of piece `index` from disk so it
    // can be served to a requesting peer. Throws if we do not have the piece or
    // the requested range falls outside the piece. Not const: read_piece may
    // lazily preallocate file handles.
    std::vector<uint8_t> read_block(uint32_t index,
                                    uint32_t begin,
                                    uint32_t length);

    // Release a claimed piece so another peer can retry it.
    // Called when a peer is choked or disconnects mid-download.
    void abort_piece(uint32_t index);

    void set_logger(std::shared_ptr<logger::Logger> logger)
    {
        m_logger = std::move(logger);
    }

    // Bytes successfully downloaded and verified so far.
    uint64_t downloaded_bytes() const
    {
        return m_curr_downloaded_bytes.load(std::memory_order_relaxed);
    }

    double percentage_complete() const
    {
        return (static_cast<double>(downloaded_bytes()) /
                static_cast<double>(m_total_length_bytes)) *
               100.0;
    }

   private:
    uint64_t m_total_length_bytes;

    // Bytes received so far; updated atomically inside complete_piece.
    std::atomic<uint64_t> m_curr_downloaded_bytes{0};

    uint32_t m_num_pieces;
    uint32_t m_nominal_piece_length;
    std::vector<crypto::SHA1Hash> m_piece_hashes;

    // m_have[i]    — piece i has been verified and written to disk.
    // m_claimed[i] — piece i is currently owned by a peer thread.
    //
    // Both are unique_ptr arrays of atomics.  std::vector<std::atomic<bool>>
    // is avoided because std::atomic is neither copy- nor move-constructible,
    // which makes the standard vector resize operations ill-formed.
    std::unique_ptr<std::atomic<bool>[]> m_have;
    std::unique_ptr<std::atomic<bool>[]> m_claimed;

    // Number of pieces that have passed hash verification.
    std::atomic<uint32_t> m_completed_count{0};

    DiskWriter& m_disk_writer;

    std::shared_ptr<logger::Logger> m_logger;
};
