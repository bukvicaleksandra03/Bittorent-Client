#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "crypto.h"
#include "logger.h"
#include "net/socket.h"
#include "peer_address.h"
#include "peer_wire/peer_message.h"
#include "peer_wire/piece_manager.h"
#include "utils.h"

class PeerConnection
{
   public:
    // lg is the per-peer Logger created by TorrentManager. Every PeerConnection
    // writes to its own file, so peer threads never share a log mutex.
    PeerConnection(PeerAddress peer,
                   crypto::SHA1Hash info_hash,
                   utils::PeerId my_peer_id,
                   PieceManager& piece_manager,
                   std::shared_ptr<logger::Logger> lg);

    ~PeerConnection();

    // TCP connect + BitTorrent handshake. Throws on failure.
    void connect(int timeout_ms = 10000);

    // Run the download loop until the PieceManager says we are done
    // or the connection dies.
    void run();

    // Immediately unblock any pending recv() by shutting down the socket.
    // Safe to call from another thread; errors are intentionally ignored.
    void cancel();

    bool is_connected() const
    {
        return m_connected;
    }

    // Total payload bytes received from this peer since the connection opened.
    // Updated atomically; safe to read from other threads without a lock.
    uint64_t bytes_downloaded() const
    {
        return m_bytes_downloaded.load(std::memory_order_relaxed);
    }

    // Total payload bytes served to this peer since the connection opened.
    // Updated atomically; safe to read from other threads without a lock.
    uint64_t bytes_uploaded() const
    {
        return m_bytes_uploaded.load(std::memory_order_relaxed);
    }

    // Number of blocks (Piece messages) served to this peer since the
    // connection opened.  Updated atomically.
    uint64_t blocks_uploaded() const
    {
        return m_blocks_uploaded.load(std::memory_order_relaxed);
    }

    // "ip:port" string for display purposes.
    std::string peer_address() const
    {
        return m_peer.to_string();
    }

   private:
    // ---- networking helpers ----
    void recv_exact(uint8_t* buf, size_t n);
    void recv_exact_timeout(uint8_t* buf, size_t n, int timeout_ms,
                            const std::string& context = "handshake");
    void send_bytes(const uint8_t* buf, size_t n);
    void send_message(const peer_wire::PeerMessage& msg);
    std::optional<peer_wire::PeerMessage> recv_message();
    std::optional<peer_wire::PeerMessage> recv_message_timeout(int timeout_ms);

    // ---- protocol helpers ----
    void do_handshake();
    void send_interested();
    void fill_requests();

    // ---- upload (seeding) helpers ----
    // Advertise the pieces we currently hold. Sent once, right after the
    // handshake, so peers know what they can request from us.
    void send_bitfield();
    // Stop choking the peer so it may begin sending us Request messages.
    void send_unchoke();

    // ---- message handlers ----
    void handle_choke();
    void handle_unchoke();
    void handle_interested();
    void handle_not_interested();
    void handle_have(const peer_wire::PeerMessage& msg);
    void handle_bitfield(const peer_wire::PeerMessage& msg);
    void handle_piece(const peer_wire::PeerMessage& msg);
    void handle_request(const peer_wire::PeerMessage& msg);
    void handle_cancel(const peer_wire::PeerMessage& msg);

    // ---- state ----
    TCPClientSocket m_socket;
    PeerAddress m_peer;

    // Total payload bytes received; updated inside handle_piece.
    std::atomic<uint64_t> m_bytes_downloaded{0};
    // Total payload bytes served; updated inside handle_request.
    std::atomic<uint64_t> m_bytes_uploaded{0};
    // Number of blocks (Piece messages) served; updated inside handle_request.
    std::atomic<uint64_t> m_blocks_uploaded{0};
    crypto::SHA1Hash m_info_hash;
    utils::PeerId m_my_peer_id;
    bool m_connected = false;
    std::shared_ptr<logger::Logger> m_logger;

    bool m_am_choking = true;
    bool m_am_interested = false;
    bool m_peer_choking = true;
    bool m_peer_interested = false;

    std::vector<bool> m_peer_bitfield;

    PieceManager& m_piece_manager;

    // ---- in-flight piece download tracking ----

    // Maximum number of outstanding block requests we allow per peer.
    // Keeping a small pipeline improves throughput without over-buffering.
    static constexpr uint32_t MAX_PIPELINE = 5;

    // All block data for the current piece lives here — completely
    // thread-local.  PieceManager never touches this buffer; only
    // complete_piece() is called once all blocks have arrived.
    struct InFlightPiece
    {
        // The piece currently being downloaded from this peer.
        uint32_t piece_index = 0;
        // Total byte size of this piece (last piece can be shorter).
        uint32_t piece_length = 0;
        // Next byte offset within the piece to request.
        uint32_t next_block_offset = 0;
        // Number of block requests sent but not yet received.
        uint32_t requests_in_flight = 0;

        // Assembled block data — written without any lock.
        std::vector<uint8_t> data;
        // Per-block arrival flags to detect duplicates.
        std::vector<bool> blocks_received;
        uint32_t blocks_done = 0;
        uint32_t blocks_total = 0;
    };

    // At any point this connection downloads at most one piece at a time.
    // next_needed() atomically claims a piece index from PieceManager; we then
    // request its blocks in a pipelined fashion (up to MAX_PIPELINE outstanding
    // requests) and assemble them here until the piece is complete.  Once all
    // blocks arrive the piece is handed to complete_piece() for hash
    // verification and disk write, after which we claim the next piece.
    // nullopt means no piece is currently claimed from this peer.
    std::optional<InFlightPiece> m_current_piece;
};
