#pragma once

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "bencode_types.h"
#include "torrent_file.h"

class BencodeParser
{
   public:
    explicit BencodeParser(const std::string& path);

    std::unique_ptr<TorrentFile> parse();

    void print(std::ostream& os)
    {
        metadata_dict->print(os);
    }

   private:
    std::shared_ptr<BDict> metadata_dict;
    size_t pos;
    std::vector<uint8_t> metadata;
    std::vector<uint8_t> info_dict_raw_bytes;

    uint8_t get();
    uint8_t peek() const;

    std::shared_ptr<BString> parse_byte_string();

    std::shared_ptr<BType> parse_bencoding_type();

    std::shared_ptr<BInteger> parse_integer();

    std::shared_ptr<BList> parse_list();

    std::shared_ptr<BDict> parse_dictionary();
};