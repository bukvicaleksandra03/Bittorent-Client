#include "peer_wire/peer_connection.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <sys/socket.h>

#include "byte_order.h"
#include "net/socket_addresses.h"

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

PeerConnection::PeerConnection(Peer peer,
                               crypto::SHA1Hash info_hash,
                               utils::PeerId my_peer_id,
                               PieceManager& piece_manager,
                               std::shared_ptr<logger::Logger> lg)
    : m_socket(AF_INET),
      m_peer(std::move(peer)),
      m_info_hash(info_hash),
      m_my_peer_id(my_peer_id),
      m_logger(std::move(lg)),
      m_piece_manager(piece_manager)
{
    // Pre-size peer availability map to torrent piece count.
    // We keep this fixed-size and treat out-of-range piece indices as protocol
    // errors instead of growing the vector dynamically.
    m_peer_bitfield.assign(piece_manager.num_pieces(), false);
}

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------

PeerConnection::~PeerConnection()
{
    if (m_current_piece)
    {
        m_piece_manager.abort_piece(m_current_piece->piece_index);
    }
}

// ---------------------------------------------------------------------------
// Cancellation
// ---------------------------------------------------------------------------

void PeerConnection::cancel()
{
    // Shutting down the socket immediately unblocks any recv() call that
    // another thread (or this thread) is blocked inside.  SHUT_RDWR ensures
    // both the read and write sides are closed so the blocked call returns
    // with an error without waiting for the remote peer to close first.
    // Errors are intentionally ignored: the fd may already be invalid or the
    // socket may not be connected yet.
    ::shutdown(m_socket.get_fd(), SHUT_RDWR);
}

// ---------------------------------------------------------------------------
// Low-level I/O helpers
// ---------------------------------------------------------------------------

void PeerConnection::recv_exact(uint8_t* buf, size_t n)
{
    size_t total = 0;
    while (total < n)
    {
        ssize_t got = m_socket.recv(buf + total, n - total);
        if (got == 0)
        {
            LLOG_THROW(
                *m_logger,
                "peer closed connection (EOF) after " + std::to_string(total) +
                    "/" + std::to_string(n) + " bytes" +
                    (m_current_piece
                         ? "; in-flight piece=" +
                               std::to_string(m_current_piece->piece_index) +
                               " offset=" +
                               std::to_string(
                                   m_current_piece->next_block_offset) +
                               " requests_in_flight=" +
                               std::to_string(
                                   m_current_piece->requests_in_flight)
                         : "; no piece in flight"));
        }
        if (got < 0)
        {
            const int err = errno;
            LLOG_THROW(
                *m_logger,
                "recv() error after " + std::to_string(total) + "/" +
                    std::to_string(n) + " bytes: " + strerror(err) +
                    " (errno=" + std::to_string(err) + ")" +
                    (m_current_piece
                         ? "; in-flight piece=" +
                               std::to_string(m_current_piece->piece_index)
                         : "; no piece in flight"));
        }
        total += static_cast<size_t>(got);
    }
}

void PeerConnection::send_bytes(const uint8_t* buf, size_t n)
{
    m_socket.send(reinterpret_cast<const char*>(buf), n);
}

void PeerConnection::send_message(const peer_wire::PeerMessage& msg)
{
    std::vector<uint8_t> wire = msg.serialize();
    send_bytes(wire.data(), wire.size());
}

std::optional<peer_wire::PeerMessage> PeerConnection::recv_message()
{
    uint8_t len_buf[4];
    recv_exact(len_buf, 4);
    uint32_t length = byte_order::read_be32(len_buf);

    if (length == 0)
    {
        return std::nullopt;  // keep-alive
    }

    if (length > 1u << 24)
    {
        LLOG_THROW(*m_logger, "message length exceeds sanity limit");
    }

    std::vector<uint8_t> body(length);
    recv_exact(body.data(), length);

    peer_wire::PeerMessage msg;
    msg.id = static_cast<peer_wire::MessageId>(body[0]);
    if (length > 1)
    {
        msg.payload.assign(body.begin() + 1, body.end());
    }
    return msg;
}

// Like recv_exact but applies a single wall-clock deadline to the entire read.
// Uses poll() before each recv() so the per-call timeout shrinks as bytes
// arrive.  Throws a descriptive message on timeout or EOF so the caller can
// log it with context ("handshake timed out", "peer closed mid-handshake", …).
void PeerConnection::recv_exact_timeout(uint8_t* buf, size_t n, int timeout_ms)
{
    using clock = std::chrono::steady_clock;
    const auto deadline =
        clock::now() + std::chrono::milliseconds(timeout_ms);

    size_t total = 0;
    while (total < n)
    {
        const auto now = clock::now();
        const int remaining_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now)
                .count());

        if (remaining_ms <= 0)
        {
            LLOG_THROW(*m_logger,
                       "handshake timed out (" + std::to_string(total) + "/" +
                           std::to_string(n) + " bytes in " +
                           std::to_string(timeout_ms) + "ms)");
        }

        ssize_t got =
            m_socket.recv_with_timeout(buf + total, n - total, remaining_ms);

        if (got == 0)
        {
            LLOG_THROW(
                *m_logger,
                "peer closed connection (EOF) during handshake after " +
                    std::to_string(total) + "/" + std::to_string(n) +
                    " bytes");
        }
        total += static_cast<size_t>(got);
    }
}

// ---------------------------------------------------------------------------
// Connection + Handshake
// ---------------------------------------------------------------------------

void PeerConnection::connect(int timeout_ms)
{
    LLOG_DEBUG(*m_logger,
               "connecting (timeout=" + std::to_string(timeout_ms) + "ms)");
    IPv4Address addr(m_peer.ip, m_peer.port);
    m_socket.connect_with_timeout(addr, timeout_ms);
    LLOG_DEBUG(*m_logger, "TCP connected, starting handshake");
    do_handshake();
    m_connected = true;
    LLOG_DEBUG(*m_logger, "handshake OK");
}

void PeerConnection::do_handshake()
{
    peer_wire::Handshake out;
    out.info_hash = m_info_hash;
    out.peer_id = m_my_peer_id;

    std::vector<uint8_t> wire = out.serialize();
    send_bytes(wire.data(), wire.size());

    // Give the peer 10 s to respond.  Peers that silently ignore our
    // plaintext handshake (firewalled at app layer, connection limit,
    // etc.) would otherwise block this thread for ~130 s (OS TCP timeout).
    uint8_t reply_buf[peer_wire::HANDSHAKE_LENGTH];
    recv_exact_timeout(reply_buf, peer_wire::HANDSHAKE_LENGTH, 10000);

    // The very first byte of a plain BitTorrent handshake is always 0x13
    // (the length of the string "BitTorrent protocol").  Any other value
    // means the peer is speaking a different protocol — most commonly
    // MSE/Protocol Encryption (used by many clients to evade ISP throttling).
    // We do not support MSE, so log the byte and bail out.
    if (reply_buf[0] != peer_wire::PROTOCOL_STRING_LENGTH)
    {
        std::ostringstream oss;
        oss << "unexpected handshake header byte 0x" << std::hex
            << std::setw(2) << std::setfill('0')
            << static_cast<int>(reply_buf[0])
            << " (expected 0x13 — peer likely uses MSE/protocol encryption)";
        LLOG_THROW(*m_logger, oss.str());
    }

    peer_wire::Handshake reply = peer_wire::Handshake::deserialize(
        reply_buf, peer_wire::HANDSHAKE_LENGTH);

    if (reply.info_hash != m_info_hash)
    {
        LLOG_THROW(*m_logger, "info_hash mismatch in handshake");
    }
}

// ---------------------------------------------------------------------------
// Protocol helpers
// ---------------------------------------------------------------------------

void PeerConnection::send_interested()
{
    if (m_am_interested)
        return;
    peer_wire::PeerMessage msg;
    msg.id = peer_wire::MessageId::Interested;
    send_message(msg);
    m_am_interested = true;
}

void PeerConnection::fill_requests()
{
    if (m_peer_choking)
    {
        LLOG_DEBUG(*m_logger, "fill_requests: peer is choking us, waiting");
        return;
    }
    if (!m_am_interested)
    {
        LLOG_DEBUG(*m_logger, "fill_requests: not interested in this peer");
        return;
    }

    // Pick a new piece if we don't have one in-flight
    if (!m_current_piece)
    {
        auto pick = m_piece_manager.next_needed(m_peer_bitfield);
        if (!pick)
        {
            LLOG_DEBUG(*m_logger,
                       "fill_requests: piece manager has no piece available "
                       "from this peer (all claimed or complete)");
            return;
        }

        InFlightPiece ifp;
        ifp.piece_index = *pick;
        ifp.piece_length = m_piece_manager.piece_length(*pick);
        ifp.next_block_offset = 0;
        ifp.requests_in_flight = 0;

        // Allocate the thread-local buffer for this piece.
        uint32_t num_blocks = (ifp.piece_length + BLOCK_SIZE - 1) / BLOCK_SIZE;
        ifp.data.resize(ifp.piece_length, 0);
        ifp.blocks_received.assign(num_blocks, false);
        ifp.blocks_done = 0;
        ifp.blocks_total = num_blocks;

        m_current_piece = std::move(ifp);
        LLOG_DEBUG(*m_logger,
                   "requesting piece " + std::to_string(*pick) + " (" +
                       std::to_string(m_current_piece->piece_length) +
                       " bytes, " + std::to_string(num_blocks) + " blocks)");
    }

    auto& ifp = *m_current_piece;

    while (ifp.requests_in_flight < MAX_PIPELINE &&
           ifp.next_block_offset < ifp.piece_length)
    {
        uint32_t block_len =
            std::min(BLOCK_SIZE, ifp.piece_length - ifp.next_block_offset);

        peer_wire::RequestMessage req;
        req.index = ifp.piece_index;
        req.begin = ifp.next_block_offset;
        req.length = block_len;
        send_message(req.to_peer_message());

        ifp.next_block_offset += block_len;
        ++ifp.requests_in_flight;
    }
}

// ---------------------------------------------------------------------------
// Message handlers
// ---------------------------------------------------------------------------

void PeerConnection::handle_choke()
{
    LLOG_DEBUG(*m_logger, "peer choked us");
    m_peer_choking = true;

    if (m_current_piece)
    {
        LLOG_DEBUG(*m_logger,
                   "aborting in-flight piece " +
                       std::to_string(m_current_piece->piece_index) +
                       " due to choke");
        m_piece_manager.abort_piece(m_current_piece->piece_index);
        m_current_piece.reset();
    }
}

void PeerConnection::handle_unchoke()
{
    LLOG_DEBUG(*m_logger, "peer unchoked us");
    m_peer_choking = false;
}

void PeerConnection::handle_interested()
{
    LLOG_DEBUG(*m_logger, "peer expressed interest");
    m_peer_interested = true;
}

void PeerConnection::handle_not_interested()
{
    LLOG_DEBUG(*m_logger, "peer expressed not-interested");
    m_peer_interested = false;
}

void PeerConnection::handle_have(const peer_wire::PeerMessage& msg)
{
    // "Have" means the peer announces availability of exactly one piece index.
    auto have = peer_wire::HaveMessage::from_peer_message(msg);

    // Piece index must always fit torrent bounds. If a peer advertises an
    // out-of-range piece, it is malformed or malicious protocol behavior.
    if (have.piece_index >= m_peer_bitfield.size())
    {
        LLOG_THROW(*m_logger,
                   "peer sent out-of-range Have index: " +
                       std::to_string(have.piece_index));
    }

    // Record that this peer can serve this piece.
    m_peer_bitfield[have.piece_index] = true;
    LLOG_DEBUG(*m_logger, "peer has piece " + std::to_string(have.piece_index));

    // If we still need this piece, advertise interest so requests can start
    // once the peer unchokes us.
    if (!m_piece_manager.have_piece(have.piece_index))
    {
        send_interested();
    }
}

void PeerConnection::handle_bitfield(const peer_wire::PeerMessage& msg)
{
    // Bitfield provides a snapshot of which pieces the peer currently has.
    auto bf = peer_wire::BitfieldMessage::from_peer_message(msg);
    uint32_t num = m_piece_manager.num_pieces();

    if (m_peer_bitfield.size() != num)
    {
        LLOG_THROW(*m_logger,
                   "peer bitfield size mismatch: local=" +
                       std::to_string(m_peer_bitfield.size()) +
                       ", expected=" + std::to_string(num));
    }

    // Copy peer availability into our fixed-size map; has_piece(i) is bounds
    // safe against the received bitfield payload.
    uint32_t peer_piece_count = 0;
    for (uint32_t i = 0; i < num; ++i)
    {
        if (bf.has_piece(i))
        {
            m_peer_bitfield[i] = true;
            ++peer_piece_count;
        }
    }
    LLOG_DEBUG(*m_logger,
               "bitfield received: peer has " +
                   std::to_string(peer_piece_count) + "/" +
                   std::to_string(num) + " pieces");

    for (uint32_t i = 0; i < num; ++i)
    {
        if (m_peer_bitfield[i] && !m_piece_manager.have_piece(i))
        {
            // Peer has at least one piece we are missing.
            send_interested();
            break;
        }
    }
}

void PeerConnection::handle_piece(const peer_wire::PeerMessage& msg)
{
    auto piece = peer_wire::PieceMessage::from_peer_message(msg);

    if (!m_current_piece || piece.index != m_current_piece->piece_index)
    {
        LLOG_WARNING(*m_logger,
                     "Ignoring stale/unexpected Piece block: index=" +
                         std::to_string(piece.index) +
                         (m_current_piece
                              ? ", expected_index=" +
                                    std::to_string(m_current_piece->piece_index)
                              : ", no piece currently in flight"));
        return;
    }

    --m_current_piece->requests_in_flight;

    auto& ifp = *m_current_piece;

    // Bounds check against our local buffer.
    if (piece.begin + piece.block.size() > ifp.data.size())
    {
        LLOG_WARNING(*m_logger,
                     "Ignoring out-of-bounds block for piece " +
                         std::to_string(piece.index) +
                         ": begin=" + std::to_string(piece.begin) +
                         " len=" + std::to_string(piece.block.size()) +
                         " piece_size=" + std::to_string(ifp.data.size()));
        return;
    }

    uint32_t block_idx = piece.begin / BLOCK_SIZE;
    if (block_idx >= ifp.blocks_received.size() ||
        ifp.blocks_received[block_idx])
    {
        LLOG_DEBUG(*m_logger,
                   "Ignoring invalid/duplicate block for piece " +
                       std::to_string(piece.index) +
                       ": block_idx=" + std::to_string(block_idx));
        return;
    }

    // ---------------------------------------------------------------------------
    // Write the block into the thread-local buffer — no lock, no shared state.
    // ---------------------------------------------------------------------------
    std::memcpy(
        ifp.data.data() + piece.begin, piece.block.data(), piece.block.size());
    ifp.blocks_received[block_idx] = true;
    ++ifp.blocks_done;

    // Account for bytes received from this peer. All duplicate/out-of-bounds
    // blocks are rejected above, so every byte counted here is genuinely new.
    m_bytes_downloaded.fetch_add(piece.block.size(), std::memory_order_relaxed);

    LLOG_DEBUG(*m_logger,
               "block received: piece=" + std::to_string(piece.index) +
                   " offset=" + std::to_string(piece.begin) +
                   " len=" + std::to_string(piece.block.size()) + " (" +
                   std::to_string(ifp.blocks_done) + "/" +
                   std::to_string(ifp.blocks_total) + " blocks)");

    if (ifp.blocks_done < ifp.blocks_total)
    {
        return;  // still waiting for more blocks from the pipeline
    }

    // All blocks assembled.  Hand off to PieceManager for hash verification
    // and disk scheduling.  complete_piece() performs only atomic updates —
    // still no mutex contention.
    bool ok = m_piece_manager.complete_piece(ifp.piece_index, ifp.data);
    if (ok)
    {
        LLOG_DEBUG(*m_logger,
                   "piece " + std::to_string(ifp.piece_index) +
                       " verified and written");
    }
    else
    {
        LLOG_WARNING(*m_logger,
                     "piece " + std::to_string(ifp.piece_index) +
                         " failed hash verification; will be retried by "
                         "another peer");
    }
    m_current_piece.reset();
}

// ---------------------------------------------------------------------------
// Main download loop
// ---------------------------------------------------------------------------

void PeerConnection::run()
{
    LLOG_DEBUG(*m_logger, "download loop started");
    while (!m_piece_manager.is_complete())
    {
        auto maybe_msg = recv_message();
        if (!maybe_msg)
            continue;  // keep-alive

        const auto& msg = *maybe_msg;
        switch (msg.id)
        {
            case peer_wire::MessageId::Choke:
                handle_choke();
                break;
            case peer_wire::MessageId::Unchoke:
                handle_unchoke();
                break;
            case peer_wire::MessageId::Interested:
                handle_interested();
                break;
            case peer_wire::MessageId::NotInterested:
                handle_not_interested();
                break;
            case peer_wire::MessageId::Have:
                handle_have(msg);
                break;
            case peer_wire::MessageId::Bitfield:
                handle_bitfield(msg);
                break;
            case peer_wire::MessageId::Piece:
                handle_piece(msg);
                break;
            case peer_wire::MessageId::Request:
                LLOG_DEBUG(*m_logger,
                           "Received Request message; seeding/upload path not "
                           "implemented, ignoring");
                break;
            case peer_wire::MessageId::Cancel:
                LLOG_DEBUG(*m_logger,
                           "Received Cancel message; seeding/upload path not "
                           "implemented, ignoring");
                break;  // seeding not implemented yet
        }

        fill_requests();
    }
}
