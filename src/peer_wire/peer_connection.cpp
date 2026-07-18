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

PeerConnection::PeerConnection(PeerAddress peer,
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
    try
    {
        m_socket.recv_exact(buf, n);
    }
    catch (const std::runtime_error& e)
    {
        LLOG_THROW(
            *m_logger,
            std::string(e.what()) +
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

// Same as recv_message() but each recv_exact call is bounded by timeout_ms.
// If the peer stops sending for longer than timeout_ms, an exception is thrown,
// which propagates out of run(), triggers the PeerConnection destructor, and
// releases any claimed piece back to PieceManager via abort_piece().
std::optional<peer_wire::PeerMessage>
PeerConnection::recv_message_timeout(int timeout_ms)
{
    uint8_t len_buf[4];
    recv_exact_timeout(len_buf, 4, timeout_ms, "message length");
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
    recv_exact_timeout(body.data(), length, timeout_ms, "message body");

    peer_wire::PeerMessage msg;
    msg.id = static_cast<peer_wire::MessageId>(body[0]);
    if (length > 1)
    {
        msg.payload.assign(body.begin() + 1, body.end());
    }
    return msg;
}

// Thin wrapper around TCPDataSocket::recv_exact_timeout that adds context
// ("handshake", "message length", …) to any exception before re-throwing.
void PeerConnection::recv_exact_timeout(uint8_t* buf, size_t n, int timeout_ms,
                                        const std::string& context)
{
    try
    {
        m_socket.recv_exact_timeout(buf, n, timeout_ms);
    }
    catch (const std::runtime_error& e)
    {
        LLOG_THROW(*m_logger, context + ": " + e.what());
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

    // Immediately after the handshake, advertise which pieces we hold so the
    // peer can decide whether it wants to download from us.  Per the protocol
    // a Bitfield, if sent at all, must be the very first message after the
    // handshake.
    send_bitfield();
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
// Upload (seeding) helpers
// ---------------------------------------------------------------------------

void PeerConnection::send_bitfield()
{
    std::vector<bool> have = m_piece_manager.current_bitfield();

    uint32_t have_count = 0;
    for (bool b : have)
    {
        if (b)
            ++have_count;
    }

    // Sending an all-zero bitfield is legal but pointless; peers treat a
    // missing bitfield the same as "has nothing", so skip it when we have no
    // pieces yet (e.g. we just started downloading).
    if (have_count == 0)
    {
        LLOG_DEBUG(*m_logger,
                   "not sending bitfield (no pieces available yet)");
        return;
    }

    // Pack the boolean vector MSB-first into bytes, matching the wire format
    // expected by BitfieldMessage::has_piece (bit 7 = lowest piece index).
    const size_t num_bytes = (have.size() + 7) / 8;
    peer_wire::BitfieldMessage bf;
    bf.bitfield.assign(num_bytes, 0);
    for (uint32_t i = 0; i < have.size(); ++i)
    {
        if (have[i])
        {
            bf.bitfield[i / 8] |= static_cast<uint8_t>(1u << (7 - (i % 8)));
        }
    }

    send_message(bf.to_peer_message());
    LLOG_DEBUG(*m_logger,
               "sent bitfield advertising " + std::to_string(have_count) + "/" +
                   std::to_string(have.size()) + " pieces");
}

void PeerConnection::send_unchoke()
{
    if (!m_am_choking)
        return;
    peer_wire::PeerMessage msg;
    msg.id = peer_wire::MessageId::Unchoke;
    send_message(msg);
    m_am_choking = false;
    LLOG_DEBUG(*m_logger, "unchoked peer");
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

    // Minimal seeding policy: unchoke any peer that becomes interested so it
    // can start requesting blocks.  A real tit-for-tat / optimistic-unchoke
    // scheduler is a later refinement.
    send_unchoke();
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

void PeerConnection::handle_request(const peer_wire::PeerMessage& msg)
{
    auto req = peer_wire::RequestMessage::from_peer_message(msg);

    // If we are still choking this peer it must not be sending requests; a
    // well-behaved peer waits for our Unchoke.  Drop the request silently
    // rather than serving data we have not agreed to send.
    if (m_am_choking)
    {
        LLOG_DEBUG(*m_logger,
                   "ignoring Request while choking peer: index=" +
                       std::to_string(req.index));
        return;
    }

    // Reject blocks larger than the protocol's accepted maximum (16 KiB).
    // Most clients close the connection on oversized requests; we simply
    // refuse to serve them to avoid being used as an amplification vector.
    if (req.length == 0 || req.length > BLOCK_SIZE)
    {
        LLOG_WARNING(*m_logger,
                     "rejecting Request with bad length=" +
                         std::to_string(req.length) +
                         " (index=" + std::to_string(req.index) + ")");
        return;
    }

    // read_block validates index/begin/length against the piece bounds and
    // that we actually hold the piece; it throws otherwise.  Treat a failed
    // read as a protocol/availability error and skip serving this block
    // instead of tearing down the whole connection.
    std::vector<uint8_t> block;
    try
    {
        block = m_piece_manager.read_block(req.index, req.begin, req.length);
    }
    catch (const std::exception& e)
    {
        LLOG_WARNING(*m_logger,
                     std::string("cannot serve requested block: ") + e.what());
        return;
    }

    peer_wire::PieceMessage piece;
    piece.index = req.index;
    piece.begin = req.begin;
    piece.block = std::move(block);
    send_message(piece.to_peer_message());

    m_bytes_uploaded.fetch_add(req.length, std::memory_order_relaxed);
    m_blocks_uploaded.fetch_add(1, std::memory_order_relaxed);

    LLOG_DEBUG(*m_logger,
               "served block: piece=" + std::to_string(req.index) +
                   " offset=" + std::to_string(req.begin) +
                   " len=" + std::to_string(req.length));
}

void PeerConnection::handle_cancel(const peer_wire::PeerMessage& msg)
{
    // We serve requests synchronously inside handle_request, so by the time a
    // Cancel could arrive the block has already been sent.  There is nothing
    // queued to cancel; just log it.
    auto cancel = peer_wire::CancelMessage::from_peer_message(msg);
    LLOG_DEBUG(*m_logger,
               "received Cancel (nothing queued): index=" +
                   std::to_string(cancel.index) +
                   " offset=" + std::to_string(cancel.begin));
}

// ---------------------------------------------------------------------------
// Main download loop
// ---------------------------------------------------------------------------

void PeerConnection::run()
{
    LLOG_DEBUG(*m_logger, "peer loop started");

    // The loop runs until the socket dies (recv throws) or cancel() shuts the
    // socket down.  We intentionally do NOT exit when the torrent is complete:
    // a finished client keeps the connection open to serve (seed) pieces to
    // this peer.  fill_requests() simply becomes a no-op once we have every
    // piece.
    //
    // 120 s matches the BitTorrent keep-alive interval (BEP 3): a live peer
    // must send at least a keep-alive message every two minutes.  If nothing
    // arrives within that window the peer is dead; the exception propagates
    // out of run(), the PeerConnection destructor fires, and abort_piece()
    // releases any claimed piece back to PieceManager so another peer can
    // pick it up.
    static constexpr int INACTIVITY_TIMEOUT_MS = 120'000;

    while (true)
    {
        auto maybe_msg = recv_message_timeout(INACTIVITY_TIMEOUT_MS);
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
                handle_request(msg);
                break;
            case peer_wire::MessageId::Cancel:
                handle_cancel(msg);
                break;
        }

        fill_requests();
    }
}
