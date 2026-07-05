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
          $(SRC_DIR)/torrent_file.cpp \
          $(SRC_DIR)/trackers/udp_tracker_communicator.cpp \
          $(SRC_DIR)/trackers/http_tracker_communicator.cpp \
          $(SRC_DIR)/peer_wire/peer_message.cpp \
          $(SRC_DIR)/peer_wire/disk_writer.cpp \
          $(SRC_DIR)/peer_wire/piece_manager.cpp \
          $(SRC_DIR)/peer_wire/peer_connection.cpp \
          $(SRC_DIR)/peer_wire/torrent_manager.cpp \
          $(SRC_DIR)/peer_wire/session_manager.cpp
OBJECTS = $(patsubst %.cpp,$(OBJ_DIR)/%.o,$(SOURCES))

.PHONY: all clean dirs test

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

# Build $(OUT_DIR)/test_* from its source + library objects. Source is found
# by basename so nested paths under tests/ still work.
$(OUT_DIR)/test_%: $(OBJECTS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INC_DIR) $(OBJECTS) \
		$$(find $(TEST_DIR) -name '$(notdir $@).cpp') -o $@ $(LDFLAGS)

# Convenience: `make test_foo` builds `out/test_foo` (same stem).
test_%: $(OUT_DIR)/test_%
	@true

# `make test` builds and runs every test binary under out/.
test: dirs $(TEST_BINS)
	@for bin in $(TEST_BINS); do \
		echo "========== Running $$bin =========="; \
		./$$bin || exit 1; \
	done

clean:
	rm -rf $(OBJ_DIR) $(OUT_DIR) $(LOG_DIR)