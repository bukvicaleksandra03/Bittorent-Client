#ifndef __BENCODE_PARSER_H_
#define __BENCODE_PARSER_H_

#include <string>
#include <vector>
#include <memory>
#include <iostream>
#include "bencode_types.h"

class BencodeParser 
{
public:
    explicit BencodeParser(const std::string& path, std::shared_ptr<BDict> metadata_dict);

    void parse();

    void print(std::ostream& os) { metadata_dict->print(os); }

private:
    int pos;
    std::vector<char> metadata;

    char get();
    char peek() const;

    std::shared_ptr<BString> parse_byte_string();

    std::shared_ptr<BType> parse_bencoding_type();

    std::shared_ptr<BInteger> parse_integer();

    std::shared_ptr<BList> parse_list();

    std::shared_ptr<BDict> parse_dictionary();

    std::shared_ptr<BDict> metadata_dict;
};

#endif // __BENCODE_PARSER_H_