CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -g -O0

LDFLAGS = -lssl -lcrypto -pthread 

# GTEST
LDFLAGS += -L/usr/lib -lgtest -lgtest_main

# Includes
INC_DIR = -I/usr/src/gtest/include
INC_DIR += -Iinc

SRC_DIR = src
OBJ_DIR = obj
TEST_DIR = tests

# Source files
SOURCES = $(SRC_DIR)/utils.cpp \
          $(SRC_DIR)/net/socket.cpp \
          $(SRC_DIR)/net/ssl_socket.cpp
OBJECTS = $(patsubst %.cpp,$(OBJ_DIR)/%.o,$(SOURCES))

# Executable names
.PHONY: all clean dirs test

all: dirs $(TARGET)

dirs:
	@mkdir -p $(OBJ_DIR)

$(OBJ_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INC_DIR) -c $< -o $@


TEST_SRC = tests/net/test_sockets.cpp
TEST_BIN = test_sockets

# Build test binary
$(TEST_BIN): $(OBJECTS) $(TEST_SRC)
	$(CXX) $(CXXFLAGS) $(INC_DIR) $^ -o $@ $(LDFLAGS)

# Run tests
test: $(TEST_BIN)
	./$(TEST_BIN)

clean:
	rm -rf $(OBJ_DIR)