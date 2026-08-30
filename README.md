# BitTorrent Client

A C++17 BitTorrent client built from scratch for learning and experimentation. It parses `.torrent` files ([BEP 3](https://www.bittorrent.org/beps/bep_0003.html)), connects to HTTP ([BEP 3](https://www.bittorrent.org/beps/bep_0003.html)) and UDP ([BEP 15](https://www.bittorrent.org/beps/bep_0015.html)) trackers, downloads over the peer wire protocol ([BEP 3](https://www.bittorrent.org/beps/bep_0003.html)), and includes a Kademlia DHT ([BEP 5](https://www.bittorrent.org/beps/bep_0005.html)) implementation for peer discovery.

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
| **DHT persistence** | `RoutingTableStore` saves/loads k-buckets between runs (warm start when enough peers respond) |
| **Run metrics** | JSON summaries per torrent, DHT session, and parallel batch (`logs/metrics/`) |
| **Networking** | TCP/UDP sockets, OpenSSL TLS wrapper, UPnP (miniupnpc-style port forward) |
| **Logging** | Structured file logging per torrent/session |
| **Tests** | Google Test binaries under `tests/` (unit, loopback, integration) |

## Project layout

```
inc/                    Public headers
  bencode/              Bencode types, parser, encoder
  dht/                  DHT client, KRPC, routing table, routing table store, lookups
  net/                  Sockets, SSL, UPnP
  peer_wire/            Messages, connections, piece manager, session
  trackers/             HTTP/UDP tracker communicators
src/                    Implementations (mirrors inc/ layout)
tests/                  Google Test sources (mirrors components)
torrent_files/          Sample torrents and download output
routing_table_store/    Persisted DHT routing table (default: routing_table.txt)
logs/                   Runtime logs and metrics JSON (gitignored content)
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
  ├── RoutingTableStore       load/save routing table to disk (warm start)
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

**Routing table persistence (`RoutingTableStore`)**

`RoutingTableStore` (`inc/dht/routing_table_store.h`) writes the current k-buckets to a text file and reloads them on the next `DhtClient::start()`. This avoids cold bootstrap on every run when enough stored nodes still respond to ping.

| Item | Detail |
|------|--------|
| Default path | `routing_table_store/routing_table.txt` |
| Override | `DHT_ROUTING_TABLE_PATH` env or `DhtClient::set_routing_table_path()` |
| Clear on start | `DHT_CLEAR_ROUTING=1` or `set_clear_persisted_routing_table(true)` |
| Warm-start rule | After load, `DhtClient` pings stored peers; if at least **4** are verified good, public bootstrap is skipped |
| Loaded entries | Restored nodes are not marked good until contact succeeds |

On shutdown, `DhtClient::stop()` saves the table when persistence is enabled. Integration scripts (`download_torrent.sh`, `download_parallel_torrents.sh`) point `DHT_ROUTING_TABLE_PATH` at the project copy under `routing_table_store/`.

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
# Download one torrent end-to-end (default: linuxmint-22.2-cinnamon-64bit.iso.torrent)
./download_torrent.sh [torrent-name]

# Download multiple torrents in parallel (one process, shared SessionManager / DHT / listen port)
./download_parallel_torrents.sh [count]
# Default count=3 uses built-in torrents: kali, debian amd64 netinst, debian mac netinst

# DHT peer discovery against a real swarm
./dht_test.sh
RUN_DHT_PEER_DISCOVERY_TEST=1 ./out/test_dht_get_peers_from_torrent
```

Sample `.torrent` files live in `torrent_files/unparsed_torrents/`. Single downloads go to `torrent_files/downloaded/`; parallel runs use `torrent_files/downloaded/parallel/<torrent-name>/`.

**Single torrent** (`download_torrent.sh` / `test_single_torrent`):

| Variable | Purpose |
|----------|---------|
| `TORRENT_PATH` | Full path to one `.torrent` (set by script) |
| `DHT_ROUTING_TABLE_PATH` | Persisted routing table path |
| `DHT_CLEAR_ROUTING=1` | Delete store and force bootstrap |
| `TORRENT_SKIP_REFERENCE=1` | Skip qBittorrent reference file check |
| `SINGLE_TORRENT_MAX_SEC` | Wall-clock limit (`0` = until complete) |

**Parallel torrents** (`download_parallel_torrents.sh` / `test_parallel_torrents`):

| Variable | Purpose |
|----------|---------|
| `PARALLEL_TORRENT_COUNT` | How many torrents to run (default `3`; uses first N from the default set) |
| `TORRENT_PATHS` | Colon-separated list of `.torrent` paths (overrides defaults) |
| `TORRENT_DIR` | Scan a directory for `.torrent` files instead of the built-in default set |
| `TORRENT_TEST_OUT` | Base download directory (per-torrent subdirs) |
| `TORRENT_METRICS_DIR` | JSON metrics output (default `logs/metrics`) |
| `TORRENT_SKIP_REFERENCE` | Default `1` in the shell script |
| `PARALLEL_TORRENT_MAX_SEC` | Wall-clock limit (`0` = until all complete) |
| `DHT_ROUTING_TABLE_PATH` / `DHT_CLEAR_ROUTING` | Same as single-torrent runs |

Built-in default torrents (when `TORRENT_PATHS` and `TORRENT_DIR` are unset):

- `kali-linux-2026.2-installer-amd64.iso.torrent`
- `debian-13.5.0-amd64-netinst.iso.torrent`
- `debian-mac-13.5.0-amd64-netinst.iso.torrent`

Run the test binary directly:

```bash
RUN_PARALLEL_TORRENTS_TEST=1 ./out/test_parallel_torrents
```
