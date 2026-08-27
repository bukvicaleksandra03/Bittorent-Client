#!/usr/bin/env bash
# Build and run the parallel-torrent integration test (3 downloads at once
# in one process, shared SessionManager / DHT / listen port).
#
# Usage:
#   ./download_parallel_torrents.sh [count]
#
# count defaults to 3. Without overrides, the test uses its built-in default
# set (kali + debian amd64 + debian mac netinst under unparsed_torrents).
#
# Override selection with TORRENT_PATHS (colon-separated full paths):
#   TORRENT_PATHS="a.torrent:b.torrent:c.torrent" ./download_parallel_torrents.sh
#
# Other useful environment variables:
#   PARALLEL_TORRENT_COUNT=3
#   TORRENT_DIR=...           # scan directory instead of built-in defaults
#   TORRENT_TEST_OUT=...      # base download dir (per-torrent subdirs)
#   TORRENT_SKIP_REFERENCE=1  # default for this script
#   PARALLEL_TORRENT_MAX_SEC=0
#   DHT_ROUTING_TABLE_PATH=...
#   DHT_CLEAR_ROUTING=1
#
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COUNT="${1:-3}"

echo "Parallel download: ${COUNT} torrent(s) (built-in default set unless TORRENT_PATHS/TORRENT_DIR is set)"

make test_parallel_torrents

env PARALLEL_TORRENT_COUNT="${COUNT}" \
    TORRENT_TEST_OUT="${PROJECT_DIR}/torrent_files/downloaded/parallel" \
    TORRENT_METRICS_DIR="${PROJECT_DIR}/logs/metrics" \
    TORRENT_SKIP_REFERENCE="${TORRENT_SKIP_REFERENCE:-1}" \
    DHT_ROUTING_TABLE_PATH="${PROJECT_DIR}/routing_table_store/routing_table.txt" \
    RUN_PARALLEL_TORRENTS_TEST=1 \
    "${PROJECT_DIR}/out/test_parallel_torrents"
