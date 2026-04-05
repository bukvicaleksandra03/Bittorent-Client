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
TEST_DIR = tests

# Source files
SOURCES = $(SRC_DIR)/net/socket.cpp \
          $(SRC_DIR)/net/ssl_socket.cpp \
          $(SRC_DIR)/bencode/bencode_parser.cpp \
          $(SRC_DIR)/torrent_file.cpp
OBJECTS = $(patsubst %.cpp,$(OBJ_DIR)/%.o,$(SOURCES))

# Executable names
.PHONY: all clean dirs test test_%

all: dirs $(TARGET)

dirs:
	@mkdir -p $(OBJ_DIR)

$(OBJ_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INC_DIR) -c $< -o $@

# Discover every tests/**/test_*.cpp and derive a binary name from it.
# tests/net/test_sockets.cpp      -> test_sockets
# tests/net/test_ssl_sockets.cpp  -> test_ssl_sockets
TEST_SRCS = $(shell find $(TEST_DIR) -name 'test_*.cpp')
TEST_BINS = $(patsubst %.cpp,%,$(notdir $(TEST_SRCS)))

# Pattern rule: build any test binary from its source + the library objects.
# The matching source is located with a recursive find so the flat binary
# name works regardless of subdirectory depth.
test_%: $(OBJECTS)
	$(CXX) $(CXXFLAGS) $(INC_DIR) $(OBJECTS) \
		$$(find $(TEST_DIR) -name '$@.cpp') -o $@ $(LDFLAGS)

# `make test` builds and runs every test binary.
test: $(TEST_BINS)
	@for bin in $(TEST_BINS); do \
		echo "========== Running $$bin =========="; \
		./$$bin || exit 1; \
	done

clean:
	rm -rf $(OBJ_DIR) $(TEST_BINS)