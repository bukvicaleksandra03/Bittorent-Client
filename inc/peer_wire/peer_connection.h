#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "crypto.h"
#include "net/socket.h"
#include "peer.h"
#include "peer_wire/peer_message.h"
#include "peer_wire/piece_manager.h"
#include "utils.h"

class PeerConnection
{
   public:
    PeerConnection(Peer peer,
                   crypto::SHA1Hash info_hash,
                   utils::PeerId my_peer_id,
                   PieceManager& piece_manager);

    // TCP connect + BitTorrent handshake. Throws on failure.
    void connect(int timeout_ms = 5000);

    // Run the download loop until the PieceManager says we are done
    // or the connection dies.
    void run();

    bool is_connected() const
    {
        return m_connected;
    }

   private:
    // ---- networking helpers ----
    void recv_exact(uint8_t* buf, size_t n);
    void send_bytes(const uint8_t* buf, size_t n);
    void send_message(const peer_wire::PeerMessage& msg);
    std::optional<peer_wire::PeerMessage> recv_message();

    // ---- protocol helpers ----
    void do_handshake();
    void send_interested();
    void fill_requests();

    // ---- message handlers ----
    void handle_choke();
    void handle_unchoke();
    void handle_interested();
    void handle_not_interested();
    void handle_have(const peer_wire::PeerMessage& msg);
    void handle_bitfield(const peer_wire::PeerMessage& msg);
    void handle_piece(const peer_wire::PeerMessage& msg);

    // ---- state ----
    TCPClientSocket m_socket;
    Peer m_peer;
    crypto::SHA1Hash m_info_hash;
    utils::PeerId m_my_peer_id;
    bool m_connected = false;

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
    };
    std::optional<InFlightPiece> m_current_piece;
};
