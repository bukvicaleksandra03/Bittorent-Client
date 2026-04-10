#include "peer_wire/peer_connection.h"

#include <algorithm>
#include <cstring>

#include "byte_order.h"
#include "logger.h"
#include "net/socket_addresses.h"

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

PeerConnection::PeerConnection(Peer peer,
                               crypto::SHA1Hash info_hash,
                               utils::PeerId my_peer_id,
                               PieceManager& piece_manager)
    : m_socket(AF_INET),
      m_peer(std::move(peer)),
      m_info_hash(info_hash),
      m_my_peer_id(my_peer_id),
      m_piece_manager(piece_manager)
{
    // Pre-size peer availability map to torrent piece count.
    // We keep this fixed-size and treat out-of-range piece indices as protocol
    // errors instead of growing the vector dynamically.
    m_peer_bitfield.assign(piece_manager.num_pieces(), false);
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
        if (got <= 0)
        {
            LOG_AND_THROW("peer connection closed");
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
        LOG_AND_THROW("message length exceeds sanity limit");
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

// ---------------------------------------------------------------------------
// Connection + Handshake
// ---------------------------------------------------------------------------

void PeerConnection::connect(int timeout_ms)
{
    IPv4Address addr(m_peer.ip, m_peer.port);
    m_socket.connect_with_timeout(addr, timeout_ms);
    do_handshake();
    m_connected = true;
}

void PeerConnection::do_handshake()
{
    peer_wire::Handshake out;
    out.info_hash = m_info_hash;
    out.peer_id = m_my_peer_id;

    std::vector<uint8_t> wire = out.serialize();
    send_bytes(wire.data(), wire.size());

    uint8_t reply_buf[peer_wire::HANDSHAKE_LENGTH];
    recv_exact(reply_buf, peer_wire::HANDSHAKE_LENGTH);

    peer_wire::Handshake reply = peer_wire::Handshake::deserialize(
        reply_buf, peer_wire::HANDSHAKE_LENGTH);

    if (reply.info_hash != m_info_hash)
    {
        LOG_AND_THROW("info_hash mismatch in handshake");
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
    if (m_peer_choking || !m_am_interested)
        return;

    // Pick a new piece if we don't have one in-flight
    if (!m_current_piece)
    {
        auto pick = m_piece_manager.next_needed(m_peer_bitfield);
        if (!pick)
            return;

        InFlightPiece ifp;
        ifp.piece_index = *pick;
        ifp.piece_length = m_piece_manager.piece_length(*pick);
        ifp.next_block_offset = 0;
        ifp.requests_in_flight = 0;
        m_current_piece = ifp;
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
    m_peer_choking = true;

    if (m_current_piece)
    {
        m_piece_manager.abort_piece(m_current_piece->piece_index);
        m_current_piece.reset();
    }
}

void PeerConnection::handle_unchoke()
{
    m_peer_choking = false;
}

void PeerConnection::handle_interested()
{
    m_peer_interested = true;
}

void PeerConnection::handle_not_interested()
{
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
        LOG_AND_THROW("peer sent out-of-range Have index: " +
                      std::to_string(have.piece_index));
    }

    // Record that this peer can serve this piece.
    m_peer_bitfield[have.piece_index] = true;

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
        LOG_AND_THROW("peer bitfield size mismatch: local=" +
                      std::to_string(m_peer_bitfield.size()) +
                      ", expected=" + std::to_string(num));
    }

    // Copy peer availability into our fixed-size map; has_piece(i) is bounds
    // safe against the received bitfield payload.
    for (uint32_t i = 0; i < num; ++i)
    {
        if (bf.has_piece(i))
        {
            m_peer_bitfield[i] = true;
        }
    }

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
        LOG_WARNING("Ignoring stale/unexpected Piece block: index=" +
                    std::to_string(piece.index) +
                    (m_current_piece
                         ? ", expected_index=" +
                               std::to_string(m_current_piece->piece_index)
                         : ", no piece currently in flight"));
        return;  // stale or unexpected block
    }

    --m_current_piece->requests_in_flight;

    bool complete = m_piece_manager.receive_block(
        piece.index, piece.begin, piece.block.data(), piece.block.size());

    if (complete)
    {
        m_current_piece.reset();
    }
}

// ---------------------------------------------------------------------------
// Main download loop
// ---------------------------------------------------------------------------

void PeerConnection::run()
{
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
                LOG_D(
                    "Received Request message; seeding/upload path not "
                    "implemented, ignoring");
                break;
            case peer_wire::MessageId::Cancel:
                LOG_D(
                    "Received Cancel message; seeding/upload path not "
                    "implemented, ignoring");
                break;  // seeding not implemented yet
        }

        fill_requests();
    }
}
