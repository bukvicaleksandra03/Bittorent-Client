

all:
	g++ -std=c++17 -Wall -Wextra -g -O0 -Iinc -o bencode_test main.cpp src/bencode_parser.cpp src/torrent_file.cpp src/utils.cpp