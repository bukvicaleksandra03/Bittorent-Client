# BitTorrent Client

A C++17 BitTorrent client built from scratch for learning and experimentation. It parses `.torrent` files, talks to HTTP/UDP trackers, downloads over the peer wire protocol, and includes a Kademlia DHT implementation for peer discovery.

## Features

### Implemented

| Area | Description |
|------|-------------|
| **Bencode** | Parser and encoder for torrent metadata (`inc/bencode/`) |
| **Torrent files** | Load and validate `.torrent` metadata, piece hashes, file layout |
| **Trackers** | HTTP and UDP announce/scrape (`inc/trackers/`) |
| **Peer wire** | Handshake, choke/interested, request/piece, bitfield, have, keep-alive |
| **Download pipeline** | Piece manager, disk writer, multi-peer workers per torrent |
| **Session management** | Multiple torrents on one listen port, UPnP port mapping, live status display |
| **DHT (BEP 5)** | KRPC, routing table, node lookup, `get_peers`, `announce_peer`, token secrets |
| **Networking** | TCP/UDP sockets, OpenSSL TLS wrapper, UPnP (miniupnpc-style port forward) |
| **Logging** | Structured file logging per torrent/session |
| **Tests** | Google Test binaries under `tests/` (unit, loopback, integration) |

## Project layout

```
inc/                    Public headers
  bencode/              Bencode types, parser, encoder
  dht/                  DHT client, KRPC, routing table, lookups
  net/                  Sockets, SSL, UPnP
  peer_wire/            Messages, connections, piece manager, session
  trackers/             HTTP/UDP tracker communicators
src/                    Implementations (mirrors inc/ layout)
tests/                  Google Test sources (mirrors components)
torrent_files/          Sample torrents and download output
logs/                   Runtime logs (gitignored content)
obj/                    Build objects (generated)
out/                    Test binaries (generated)
```

## Architecture (high level)

```
SessionManager
  ├── UPnP mapping (one port for all torrents)
  ├── DhtClient (UDP Kademlia, BEP 5 — same listen port as BitTorrent)
  └── TorrentManager (one per .torrent)
        ├── TrackerCommunicator (HTTP/UDP announce)
        ├── PieceManager + DiskWriter
        └── PeerConnection workers (TCP peer wire)
```

- **SessionManager** owns multiple torrents, shares a listen port, and can refresh a terminal status view. It starts `DhtClient` on that port, registers each active torrent for periodic DHT refresh, and routes discovered peers into the matching `TorrentManager` via a callback.
- **TorrentManager** runs tracker announce, connects to peers (trackers + DHT), requests pieces, verifies SHA-1, and writes to disk.

### DhtClient (BEP 5 Kademlia)

`DhtClient` is the public entry point for the DHT subsystem (`inc/dht/dht_client.h`). It binds a UDP socket, runs two background threads, and coordinates the components below. It does not perform TCP peer wire I/O; it only discovers peers and announces our presence on the DHT.

```
DhtClient
  ├── UDPSocket + KRPC codec (bencode over UDP)
  ├── recv_thread_            receives and handles messages
  ├── maint_thread_           bootstrap, bucket refresh, registered-torrent upkeep
  ├── RoutingTable            160 k-buckets × k=8 (XOR distance to self_id)
  ├── GetPeersLookupManager   iterative get_peers per info hash
  │     └── KademliaLookup    
  ├── DhtPeerStore            LRU peers per info hash (30 min TTL)
  ├── AnnounceCoordinator     registered torrents + tokens from responses
  └── TokenSecretRotator      BEP-5 announce tokens (SHA1, 5 min rotation)
```

**Peer discovery (`get_peers`)**

1. `SessionManager` calls `register_torrent(info_hash, listen_port)` or `get_peers(info_hash)`.
2. `GetPeersLookupManager::start_or_advance()` seeds candidates from `RoutingTable::closest(info_hash)`.
3. Up to **α=6** parallel `get_peers` queries go out; each txn is stored in `pending_lookups_`.
4. Responses with **values** (compact peers) are stored in `DhtPeerStore` and passed to the **peer callback**.
5. Responses with **nodes** extend the Kademlia candidate list until the lookup finishes or stalls (TTL expiry).

**Announce (`announce_peer`)**

1. Remote nodes that answer our `get_peers` may return a **token**; `AnnounceCoordinator` caches `(PeerAddress → token)` per info hash.
2. For torrents registered via `register_torrent()`, the coordinator schedules periodic refresh and emits `AnnounceRequest`s (token + listen port).
3. When acting as a DHT **server**, `on_get_peers` issues tokens via `TokenSecretRotator` (IP-bound SHA1); `on_announce_peer` verifies them before recording the peer in `DhtPeerStore`.

---

## Requirements

- Linux (developed and tested on Ubuntu/Debian)
- g++ with C++17
- OpenSSL (`libssl-dev`)
- Google Test (`libgtest-dev`, built and installed — see `dependencies.txt`)
- Optional: `clangd` for IDE support

Install build dependencies:

```bash
sudo apt install build-essential libssl-dev libgtest-dev clangd
# Build and install gtest libraries (once):
cd /usr/src/gtest && sudo cmake . && sudo make && sudo cp lib/*.a /usr/lib
```

## Build

```bash
make              # compile all object files (no standalone binary yet; tests link objects)
make test_foo     # build a single test, e.g. make test_bencode_parser
```

Object files go to `obj/`, test binaries to `out/`.

## Tests

Tests are Google Test executables. The Makefile groups them by speed:

| Target | What it runs |
|--------|----------------|
| `make test-fast` | Unit tests + local loopback (default for day-to-day work) |
| `make test-integration` | Live HTTP/UDP tracker and UPnP tests (needs network) |
| `make test` | `test-fast` then `test-integration` |

Examples:

```bash
make test-fast
make test_dht_client
./out/test_dht_client

make test-integration          # slow; hits real trackers
./out/test_dht_loopback        # two DHT nodes on localhost
```

### Manual / long-running tests

These are **not** part of `make test`:

```bash
# Download one torrent end-to-end
./download_torrent.sh [torrent-name]

# DHT peer discovery against a real swarm
./dht_test.sh
RUN_DHT_PEER_DISCOVERY_TEST=1 ./out/test_dht_get_peers_from_torrent
```

Sample `.torrent` files live in `torrent_files/unparsed_torrents/`. Downloads go to `torrent_files/downloaded/`.
