#include "bencode_parser.h"
#include <fstream>
#include <cctype>

BencodeParser::BencodeParser(const std::string& path, std::shared_ptr<BDict> metadata_dict) :
    pos(0), metadata_dict(metadata_dict)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot open file");

    metadata = {
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>()
    };
}

char
BencodeParser::get()
{
    if (pos >= metadata.size())
        throw std::runtime_error("No more metadata left.");
    
    return metadata[pos++];
}

char
BencodeParser::peek() const
{
    return metadata[pos];
}

void
BencodeParser::parse()
{
    if (get() != 'd')
        throw std::runtime_error("The content of the .torrent file must be a bencoded dictionary.");
    
    while (peek() != 'e') {
        std::shared_ptr<BString> bs = parse_byte_string();
        std::shared_ptr<BType> bt = parse_bencoding_type();

        metadata_dict->content[bs->content] = std::move(bt);
    }

    if (get() != 'e')
        throw std::runtime_error("Dictionary not properly terminated.");
}

std::shared_ptr<BString>
BencodeParser::parse_byte_string()
{
    int i = 0;
    for (i = pos; i < metadata.size(); i++) {
        if (metadata[i] == ':') break;
        if (!std::isdigit(metadata[i]))
            throw std::runtime_error("Error parsing the length of the string at postion " + i + '.');
    }

    std::string numStr(metadata.begin() + pos, metadata.begin() + i);
    int length = std::stoi(numStr);
    pos = i;

    if (get() != ':')
        throw std::runtime_error("Missing \":\" at pos " + pos + '.');

    auto b = std::make_shared<BString>();
    if (pos + length > metadata.size())
        throw std::runtime_error("String at the end of the file wasn't finished at position " + pos + '.');

    b->content = std::string(metadata.begin() + pos, metadata.begin() + pos + length);
    pos = pos + length;
    return b;
}

std::shared_ptr<BType>
BencodeParser::parse_bencoding_type()
{
    char nextChar = peek();
    switch(nextChar) {
        case 'i': return parse_integer();
        case 'l': return parse_list();
        case 'd': return parse_dictionary();
    }

    if (std::isdigit(nextChar)) return parse_byte_string();

    throw std::runtime_error("Unable to parse type at position: " + pos + '.');
}


std::shared_ptr<BInteger>
BencodeParser::parse_integer()
{
    if (get() != 'i')
        throw std::runtime_error("Integer must start with 'i'");

    int start = pos;
    while (peek() != 'e') {
        if (!std::isdigit(peek()) && peek() != '-') {
            throw std::runtime_error("Invalid character in integer at position " + std::to_string(pos));
        }
        get();
    }

    std::string numStr(metadata.begin() + start, metadata.begin() + pos);
    get(); // consume ending 'e'

    auto bint = std::make_shared<BInteger>();
    bint->value = std::stoll(numStr);
    return bint;
}

std::shared_ptr<BList> BencodeParser::parse_list()
{
    if (get() != 'l')
        throw std::runtime_error("List must start with 'l'");

    auto blist = std::make_shared<BList>();
    while (peek() != 'e') {
        blist->content.push_back(parse_bencoding_type());
    }

    if (get() != 'e')
        throw std::runtime_error("List not properly terminated.");

    return blist;
}

std::shared_ptr<BDict> BencodeParser::parse_dictionary()
{
    if (get() != 'd')
        throw std::runtime_error("Dictionary must start with 'd'");

    auto bdict = std::make_shared<BDict>();
    while (peek() != 'e') {
        auto key = parse_byte_string();
        auto value = parse_bencoding_type();

        bdict->content[key->content] = std::move(value);
    }

    if (get() != 'e') // consume ending 'e'
        throw std::runtime_error("Dictionary not properly terminated.");

    return bdict;
}