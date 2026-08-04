#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "crypto.h"
#include "info_hash.h"
#include "utils.h"

namespace peer_wire
{

inline constexpr uint8_t PROTOCOL_STRING_LENGTH = 19;
inline constexpr char PROTOCOL_STRING[] = "BitTorrent protocol";
inline constexpr size_t HANDSHAKE_LENGTH = 1 + 19 + 8 + 20 + 20;  // 68 bytes

// Handshake: 68 bytes
//
// Offset  Size(B)    Field
//  0       1      pstrlen  (always 19)
//  1      19      pstr     ("BitTorrent protocol")
// 20       8      reserved (all zeros, for now)
// 28      20      info_hash
// 48      20      peer_id

struct Handshake
{
    std::array<uint8_t, 8>
        reserved{};  // reserved for future protocol extensions
    InfoHash info_hash;
    utils::PeerId peer_id;

    std::vector<uint8_t> serialize() const;
    static Handshake deserialize(const uint8_t* buf, size_t len);
};

enum class MessageId : uint8_t
{
    Choke = 0,
    Unchoke = 1,
    Interested = 2,
    NotInterested = 3,
    Have = 4,
    Bitfield = 5,
    Request = 6,
    Piece = 7,
    Cancel = 8,
};

// Peer message (length-prefixed):
//
// Offset  Size(B)  Field
//  0       4       length   (big-endian, = 1 + payload size)
//  4       1       id       (MessageId enum)
//  5       N       payload  (varies by message type)
//
// Keep-alive is length=0 with no id or payload; handle at the
// connection layer since this struct always carries an id.

struct PeerMessage
{
    MessageId id;
    std::vector<uint8_t>
        payload;  // empty for choke/unchoke/interested/not-interested

    std::vector<uint8_t> serialize() const;
    static PeerMessage deserialize(const uint8_t* buf, size_t len);
};

// ---- Typed message structs ------------------------------------------------
// Each wraps the raw PeerMessage payload for a specific message type.
// Use from_peer_message() to parse, to_peer_message() to build.

// have: <len=0005><id=4><piece index>
// The 'have' message's payload is a single number, the index which that
// downloader just completed and checked the hash of.
struct HaveMessage
{
    uint32_t piece_index;

    PeerMessage to_peer_message() const;
    static HaveMessage from_peer_message(const PeerMessage& msg);
};

// bitfield: <len=0001+X><id=5><bitfield>
// 'bitfield' is only ever sent as the first message. Its payload is a bitfield
// with each index that downloader has sent set to one and the rest set to zero.
// Downloaders which don't have anything yet may skip the 'bitfield' message.
// The first byte of the bitfield corresponds to indices 0 - 7 from high bit to
// low bit, respectively. The next one 8-15, etc. Spare bits at the end are set
// to zero.
struct BitfieldMessage
{
    std::vector<uint8_t> bitfield;

    bool has_piece(uint32_t index) const;
    PeerMessage to_peer_message() const;
    static BitfieldMessage from_peer_message(const PeerMessage& msg);
};

// request: <len=0013><id=6><index><begin><length>
// 'request' messages contain an index, begin, and length. The last two are byte
// offsets. Length is generally a power of two unless it gets truncated by the
// end of the file. All current implementations use 2^14 (16 kiB), and close
// connections which request an amount greater than that.
struct RequestMessage
{
    uint32_t index;   // piece number in the torrent
    uint32_t begin;   // the byte offset within that piece where the requested
                      // block starts
    uint32_t length;  // almost always 2^14 (16kiB), except possibly for the
                      // last block of a piece

    PeerMessage to_peer_message() const;
    static RequestMessage from_peer_message(const PeerMessage& msg);
};

// piece: <len=0009+X><id=7><index><begin><block>
// 'piece' messages contain an index, begin, and piece. Note that they are
// correlated with request messages implicitly. It's possible for an unexpected
// piece to arrive if choke and unchoke messages are sent in quick succession
// and/or transfer is going very slowly.
struct PieceMessage
{
    uint32_t index;  // piece number in the torrent
    uint32_t
        begin;  // the byte offset within that piece where the sent block starts
    std::vector<uint8_t> block;  // the block data

    PeerMessage to_peer_message() const;
    static PieceMessage from_peer_message(const PeerMessage& msg);
};

// cancel: <len=0013><id=8><index><begin><length>
// Cancel messages have the same payload as request messages. They are generally
// only sent towards the end of download, during what's called 'endgame mode'.
// When a download is almost complete, there's a tendency for the last few
// pieces to all be downloaded off a single hosed modem line, taking a very long
// time. To make sure the last few pieces come in quickly, once requests for all
// pieces a given downloader doesn't have yet are currently pending, it sends
// requests for everything to everyone it's downloading from. To keep this from
// becoming horribly inefficient, it sends cancels to everyone else every time a
// piece arrives.
struct CancelMessage
{
    // the same structure as Request Messages
    uint32_t index;
    uint32_t begin;
    uint32_t length;

    PeerMessage to_peer_message() const;
    static CancelMessage from_peer_message(const PeerMessage& msg);
};

}  // namespace peer_wire