make test_dht_get_peers_from_torrent

# Optional
# TORRENT_PATH
# Path to .torrent file (default: torrent_files/unparsed_torrents/ubuntu-25.10-desktop-amd64.iso.torrent)
# DHT_PEER_LOOKUP_TIMEOUT_SEC
# Lookup duration in seconds (quick: 60, long: 120)

# Quick test (stops after finding the first peer, ~60 s)
# RUN_DHT_PEER_DISCOVERY_TEST=1 ./out/test_dht_get_peers_from_torrent --gtest_filter='DhtPeerDiscovery.GetPeersFromRealTorrent'

# Long test (full lookup, writes dht.log, ~120 s)
RUN_DHT_PEER_DISCOVERY_LONG_TEST=1 ./out/test_dht_get_peers_from_torrent --gtest_filter='DhtPeerDiscovery.GetPeersFromRealTorrentLong'