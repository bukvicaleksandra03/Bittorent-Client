#include "peer_wire/peer_message.h"

#include <cstring>

#include "byte_order.h"

namespace peer_wire
{

std::vector<uint8_t> Handshake::serialize() const
{
    std::vector<uint8_t> buf;
    buf.reserve(HANDSHAKE_LENGTH);

    buf.push_back(PROTOCOL_STRING_LENGTH);
    buf.insert(
        buf.end(), PROTOCOL_STRING, PROTOCOL_STRING + PROTOCOL_STRING_LENGTH);
    buf.insert(buf.end(), reserved.begin(), reserved.end());
    buf.insert(buf.end(), info_hash.bytes.begin(), info_hash.bytes.end());
    buf.insert(buf.end(), peer_id.begin(), peer_id.end());

    return buf;
}

Handshake Handshake::deserialize(const uint8_t* data, size_t len)
{
    if (len < HANDSHAKE_LENGTH)
    {
        throw std::runtime_error("handshake too short: expected 68 bytes");
    }

    size_t offset = 0;

    const uint8_t pstrlen = data[offset++];
    if (pstrlen != PROTOCOL_STRING_LENGTH)
    {
        throw std::runtime_error("unexpected protocol string length");
    }

    if (std::memcmp(data + offset, PROTOCOL_STRING, PROTOCOL_STRING_LENGTH) !=
        0)
    {
        throw std::runtime_error("unexpected protocol string");
    }
    offset += PROTOCOL_STRING_LENGTH;

    Handshake hs{};
    std::memcpy(hs.reserved.data(), data + offset, 8);
    offset += 8;

    const auto info_hash =
        InfoHash::try_from_raw(std::string_view(
            reinterpret_cast<const char*>(data + offset), 20));
    if (!info_hash)
    {
        throw std::runtime_error("invalid info_hash length in handshake");
    }
    hs.info_hash = *info_hash;
    offset += 20;

    std::memcpy(hs.peer_id.data(), data + offset, 20);

    return hs;
}

std::vector<uint8_t> PeerMessage::serialize() const
{
    const uint32_t length = 1 + static_cast<uint32_t>(payload.size());
    std::vector<uint8_t> buf(4 + length);

    byte_order::write_be32(buf.data(), length);
    buf[4] = static_cast<uint8_t>(id);
    if (!payload.empty())
    {
        std::memcpy(buf.data() + 5, payload.data(), payload.size());
    }

    return buf;
}

PeerMessage PeerMessage::deserialize(const uint8_t* data, size_t len)
{
    if (len < 4)
    {
        throw std::runtime_error(
            "peer message too short: need at least 4 bytes for length prefix");
    }

    const uint32_t length = byte_order::read_be32(data);
    if (length == 0)
    {
        throw std::runtime_error(
            "keep-alive has no message id; handle at connection layer");
    }

    if (len < 4 + length)
    {
        throw std::runtime_error("peer message truncated");
    }

    PeerMessage msg{};
    msg.id = static_cast<MessageId>(data[4]);

    if (length > 1)
    {
        msg.payload.assign(data + 5, data + 4 + length);
    }

    return msg;
}

// ---- HaveMessage ----------------------------------------------------------

PeerMessage HaveMessage::to_peer_message() const
{
    PeerMessage msg{};
    msg.id = MessageId::Have;
    msg.payload.resize(4);
    byte_order::write_be32(msg.payload.data(), piece_index);
    return msg;
}

HaveMessage HaveMessage::from_peer_message(const PeerMessage& msg)
{
    if (msg.id != MessageId::Have)
    {
        throw std::runtime_error("expected Have message");
    }
    if (msg.payload.size() != 4)
    {
        throw std::runtime_error("have payload must be exactly 4 bytes");
    }

    return {byte_order::read_be32(msg.payload.data())};
}

// ---- BitfieldMessage ------------------------------------------------------

bool BitfieldMessage::has_piece(uint32_t index) const
{
    const uint32_t byte_idx = index / 8;
    const uint8_t bit_idx = 7 - (index % 8);  // MSB = lowest index

    if (byte_idx >= bitfield.size())
    {
        throw std::runtime_error("bitfield index out of range: index=" +
                      std::to_string(index) + ", byte_idx=" +
                      std::to_string(byte_idx) + ", bitfield_size=" +
                      std::to_string(bitfield.size()));
    }
    return (bitfield[byte_idx] >> bit_idx) & 1;
}

PeerMessage BitfieldMessage::to_peer_message() const
{
    PeerMessage msg{};
    msg.id = MessageId::Bitfield;
    msg.payload = bitfield;
    return msg;
}

BitfieldMessage BitfieldMessage::from_peer_message(const PeerMessage& msg)
{
    if (msg.id != MessageId::Bitfield)
    {
        throw std::runtime_error("expected Bitfield message");
    }

    return {msg.payload};
}

// ---- RequestMessage -------------------------------------------------------

PeerMessage RequestMessage::to_peer_message() const
{
    PeerMessage msg{};
    msg.id = MessageId::Request;
    msg.payload.resize(12);
    byte_order::write_be32(msg.payload.data(), index);
    byte_order::write_be32(msg.payload.data() + 4, begin);
    byte_order::write_be32(msg.payload.data() + 8, length);
    return msg;
}

RequestMessage RequestMessage::from_peer_message(const PeerMessage& msg)
{
    if (msg.id != MessageId::Request)
    {
        throw std::runtime_error("expected Request message");
    }
    if (msg.payload.size() != 12)
    {
        throw std::runtime_error("request payload must be exactly 12 bytes");
    }

    RequestMessage req{};
    req.index = byte_order::read_be32(msg.payload.data());
    req.begin = byte_order::read_be32(msg.payload.data() + 4);
    req.length = byte_order::read_be32(msg.payload.data() + 8);
    return req;
}

// ---- PieceMessage ---------------------------------------------------------

PeerMessage PieceMessage::to_peer_message() const
{
    PeerMessage msg{};
    msg.id = MessageId::Piece;
    msg.payload.resize(8 + block.size());
    byte_order::write_be32(msg.payload.data(), index);
    byte_order::write_be32(msg.payload.data() + 4, begin);
    if (!block.empty())
    {
        std::memcpy(msg.payload.data() + 8, block.data(), block.size());
    }
    return msg;
}

PieceMessage PieceMessage::from_peer_message(const PeerMessage& msg)
{
    if (msg.id != MessageId::Piece)
    {
        throw std::runtime_error("expected Piece message");
    }
    if (msg.payload.size() < 8)
    {
        throw std::runtime_error("piece payload must be at least 8 bytes");
    }

    PieceMessage piece{};
    piece.index = byte_order::read_be32(msg.payload.data());
    piece.begin = byte_order::read_be32(msg.payload.data() + 4);
    piece.block.assign(msg.payload.begin() + 8, msg.payload.end());
    return piece;
}

// ---- CancelMessage --------------------------------------------------------

PeerMessage CancelMessage::to_peer_message() const
{
    PeerMessage msg{};
    msg.id = MessageId::Cancel;
    msg.payload.resize(12);
    byte_order::write_be32(msg.payload.data(), index);
    byte_order::write_be32(msg.payload.data() + 4, begin);
    byte_order::write_be32(msg.payload.data() + 8, length);
    return msg;
}

CancelMessage CancelMessage::from_peer_message(const PeerMessage& msg)
{
    if (msg.id != MessageId::Cancel)
    {
        throw std::runtime_error("expected Cancel message");
    }
    if (msg.payload.size() != 12)
    {
        throw std::runtime_error("cancel payload must be exactly 12 bytes");
    }

    CancelMessage cancel{};
    cancel.index = byte_order::read_be32(msg.payload.data());
    cancel.begin = byte_order::read_be32(msg.payload.data() + 4);
    cancel.length = byte_order::read_be32(msg.payload.data() + 8);
    return cancel;
}

}  // namespace peer_wire
