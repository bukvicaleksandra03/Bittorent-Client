#!/usr/bin/env bash
# Build and run the single-torrent integration test against one .torrent file.
#
# Usage:
#   ./download_torrent.sh [torrent-name]
#
# torrent-name is the file name inside torrent_files/unparsed_torrents/.
# If omitted, DEFAULT_TORRENT below is used.
#
#   DHT_CLEAR_ROUTING=1     # delete persisted routing table and force bootstrap
#   DHT_ROUTING_TABLE_PATH=...  # override default routing_table_store/routing_table.txt
#   DHT_KEEP_LOG=1          # keep logs/dht.log across runs (batch mode)
#   TORRENT_SKIP_REFERENCE=1  # skip qBittorrent reference file comparison
#   SINGLE_TORRENT_MAX_SEC=0  # 0 = run until complete
#
#   ./download_torrent.sh                                  # uses the default
#   ./download_torrent.sh ubuntu-22.04-desktop-amd64.iso.torrent
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TORRENT_DIR="${PROJECT_DIR}/torrent_files/unparsed_torrents"
DEFAULT_TORRENT="linuxmint-22.2-cinnamon-64bit.iso.torrent"

TORRENT_NAME="${1:-${DEFAULT_TORRENT}}"
TORRENT_PATH="${TORRENT_DIR}/${TORRENT_NAME}"

if [[ ! -f "${TORRENT_PATH}" ]]; then
    echo "error: torrent file not found: ${TORRENT_PATH}" >&2
    echo "available torrents in ${TORRENT_DIR}:" >&2
    ls -1 "${TORRENT_DIR}" >&2 || true
    exit 1
fi

echo "Using torrent: ${TORRENT_PATH}"

make test_single_torrent

env TORRENT_PATH="${TORRENT_PATH}" \
    TORRENT_METRICS_DIR="${PROJECT_DIR}/logs/metrics" \
    DHT_ROUTING_TABLE_PATH="${PROJECT_DIR}/routing_table_store/routing_table.txt" \
    RUN_SINGLE_TORRENT_TEST=1 \
    "${PROJECT_DIR}/out/test_single_torrent"
