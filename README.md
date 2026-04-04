# Bittorent-Client

## Need to support

BEP 19 (WebSeed - HTTP/FTP Seeding) lets a BitTorrent client download pieces from ordinary HTTP or FTP servers, not just from other BitTorrent peers.

How it works:

The .torrent file includes a url-list key containing one or more HTTP/FTP URLs pointing to the actual file on a web server.

The BitTorrent client treats each URL as a "peer" in the swarm. When it needs a piece, it can request it from a web server using a standard HTTP Range request (e.g., Range: bytes=524288-1048575) instead of using the BitTorrent wire protocol.

The client intermixes these HTTP downloads with normal peer-to-peer transfers. It applies the same piece-hashing verification to data received from web servers as it does for data from peers.

Why it matters:

Bootstrap problem solved. A brand-new torrent with zero peers can still be downloaded immediately because the web server acts as the initial seed.
No special server software. Any standard web server (nginx, Apache, S3 bucket) works -- it just needs to support HTTP range requests, which virtually all do.
Gradual transition. As peers join the swarm, the client shifts load away from the web server to peer-to-peer, reducing server bandwidth costs over time.