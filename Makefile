CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -g -O0
LDFLAGS = -lssl -lcrypto -pthread

# Directories
INC_DIR = inc
SRC_DIR = src
OBJ_DIR = obj
TEST_DIR = tests

# Source files
SOURCES = $(SRC_DIR)/bencode_parser.cpp \
          $(SRC_DIR)/torrent_file.cpp \
          $(SRC_DIR)/utils.cpp \
          $(SRC_DIR)/socket.cpp \
          $(SRC_DIR)/ssl_socket.cpp \
          $(SRC_DIR)/tracker_request.cpp \
          $(SRC_DIR)/tracker_request_http.cpp \
          $(SRC_DIR)/tracker_request_udp.cpp \
          $(SRC_DIR)/tracker_response.cpp
OBJECTS = $(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(SOURCES))

# Executable names
TARGET = bittorrent_client
PARSER_TEST = parser_test

.PHONY: all clean dirs test

all: dirs $(TARGET)

dirs:
	@mkdir -p $(OBJ_DIR)

$(TARGET): $(OBJECTS) $(SRC_DIR)/main.cpp
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -o $@ $(SRC_DIR)/main.cpp $(OBJECTS) $(LDFLAGS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -c $< -o $@

# Parser test
$(PARSER_TEST): dirs $(OBJECTS)
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) -o $@ $(TEST_DIR)/parser_test.cpp $(OBJECTS) $(LDFLAGS)

# Run parser test
test: $(PARSER_TEST)
	./$(PARSER_TEST)

clean:
	rm -rf $(OBJ_DIR) $(TARGET) $(PARSER_TEST)