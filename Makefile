CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -g -O0

# OpenSSL
# libssl - Implements TLS/SSL protocols, handling the handshake, session management, record layer and cipher negotiation
# libcrypto - provides underlying cryptographic algorithms (AES, RSA, SHA, EC...), X.509 certificate handling and more
LDFLAGS = -lssl -lcrypto 

# PTHREAD
# At compile time - Tells the C/C++ standard library headers to use thread-safe variants of functions
# At link time - Link against the POSIX threads library (libpthread) which provides the threading API 
# 				 and underpins C++'s <thread>, <mutex>, <condition_variable>, etc. facilities on Linux
LDFLAGS += -pthread 

# GTEST
LDFLAGS += -L/usr/lib -lgtest -lgtest_main


# Includes
INC_DIR = -I/usr/src/gtest/include
INC_DIR += -Iinc

SRC_DIR = src
OBJ_DIR = obj
OUT_DIR = out
TEST_DIR = tests
LOG_DIR = logs

# Source files
SOURCES = $(SRC_DIR)/net/socket.cpp \
		 $(SRC_DIR)/net/upnp.cpp \
          $(SRC_DIR)/net/ssl_socket.cpp \
          $(SRC_DIR)/bencode/bencode_parser.cpp \
          $(SRC_DIR)/bencode/bencode_encoder.cpp \
          $(SRC_DIR)/torrent_file.cpp \
          $(SRC_DIR)/peer_address.cpp \
          $(SRC_DIR)/trackers/udp_tracker_communicator.cpp \
          $(SRC_DIR)/trackers/http_tracker_communicator.cpp \
          $(SRC_DIR)/peer_wire/peer_message.cpp \
          $(SRC_DIR)/peer_wire/disk_writer.cpp \
          $(SRC_DIR)/peer_wire/piece_manager.cpp \
          $(SRC_DIR)/peer_wire/peer_connection.cpp \
          $(SRC_DIR)/peer_wire/torrent_manager.cpp \
          $(SRC_DIR)/peer_wire/session_manager.cpp \
          $(SRC_DIR)/dht/node_id.cpp \
          $(SRC_DIR)/dht/routing_table.cpp \
          $(SRC_DIR)/dht/krpc.cpp \
          $(SRC_DIR)/dht/dht_peer_store.cpp \
          $(SRC_DIR)/dht/kademlia_lookup.cpp \
          $(SRC_DIR)/dht/get_peers_lookup_manager.cpp \
          $(SRC_DIR)/dht/token_secret_rotator.cpp \
          $(SRC_DIR)/dht/announce_coordinator.cpp \
          $(SRC_DIR)/dht/dht_client.cpp
OBJECTS = $(patsubst %.cpp,$(OBJ_DIR)/%.o,$(SOURCES))

.PHONY: all clean dirs test test-fast test-integration

all: dirs $(TARGET)

dirs:
	@mkdir -p $(OBJ_DIR) $(OUT_DIR)

$(OBJ_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INC_DIR) -c $< -o $@

# Discover every tests/**/test_*.cpp and derive a binary path under out/.
# tests/net/test_sockets.cpp      -> out/test_sockets
# tests/net/test_ssl_sockets.cpp  -> out/test_ssl_sockets
TEST_SRCS = $(shell find $(TEST_DIR) -name 'test_*.cpp')
TEST_BINS = $(addprefix $(OUT_DIR)/,$(patsubst %.cpp,%,$(notdir $(TEST_SRCS))))

# Network-dependent / slow. Use before tracker or UPnP changes: `make test-integration`.
SLOW_TEST_BINS = $(OUT_DIR)/test_tracker_communicator \
                 $(OUT_DIR)/test_udp_tracker_communicator \
                 $(OUT_DIR)/test_upnp \
                 $(OUT_DIR)/test_dht_loopback
# Manual-only: long real download; run via download_torrent.sh or env var.
MANUAL_TEST_BINS = $(OUT_DIR)/test_single_torrent \
                   $(OUT_DIR)/test_dht_get_peers_from_torrent \
                   $(OUT_DIR)/test_scrape_all_torrents
FAST_TEST_BINS = $(filter-out $(SLOW_TEST_BINS) $(MANUAL_TEST_BINS),$(TEST_BINS))

$(TEST_BINS): $(OBJECTS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INC_DIR) $(OBJECTS) \
		$$(find $(TEST_DIR) -name '$(notdir $@).cpp') -o $@ $(LDFLAGS)

# Keep binaries when building via `make test_foo` (otherwise make treats
# out/test_foo as an intermediate and deletes it after the alias target).
.SECONDARY: $(TEST_BINS)

# Convenience: `make test_foo` builds `out/test_foo` (same stem).
test_%: $(OUT_DIR)/test_%
	@true

# `make test-fast` — unit tests + local loopback (no live trackers). Default for
# small changes.
test-fast: dirs $(FAST_TEST_BINS)
	@for bin in $(FAST_TEST_BINS); do \
		echo "========== Running $$bin =========="; \
		./$$bin || exit 1; \
	done

# `make test-integration` — live HTTP/UDP tracker tests (slow, network-dependent).
test-integration: dirs $(SLOW_TEST_BINS)
	@for bin in $(SLOW_TEST_BINS); do \
		echo "========== Running $$bin =========="; \
		./$$bin || exit 1; \
	done

# `make test` — fast + integration. Does not run test_single_torrent (manual).
test: test-fast test-integration

clean:
	rm -rf $(OBJ_DIR) $(OUT_DIR) $(LOG_DIR)