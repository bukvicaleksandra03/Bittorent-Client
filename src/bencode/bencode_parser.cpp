#include "bencode/bencode_parser.h"

#include <cctype>
#include <fstream>

bencode::Parser::Parser(const std::string& path)
    : metadata_dict(std::make_shared<BDict>()), pos(0)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Cannot open file: " + path);

    // Read file into vector<uint8_t>
    // Use char iterator, then copy to uint8_t
    std::vector<char> buffer{std::istreambuf_iterator<char>(file),
                             std::istreambuf_iterator<char>()};
    metadata.assign(buffer.begin(), buffer.end());
}

bencode::Parser::Parser(const uint8_t* data, size_t length)
    : metadata_dict(std::make_shared<BDict>()), pos(0)
{
    metadata.assign(data, data + length);
}

bencode::Parser::Parser(const std::string& data, bool /*from_string*/)
    : metadata_dict(std::make_shared<BDict>()), pos(0)
{
    metadata.assign(data.begin(), data.end());
}

std::shared_ptr<BType> bencode::Parser::parse_value()
{
    return parse_bencoding_type();
}

uint8_t bencode::Parser::get()
{
    if (pos >= metadata.size())
        throw std::runtime_error("No more metadata left.");

    return metadata[pos++];
}

uint8_t bencode::Parser::peek() const
{
    if (pos >= metadata.size())
        throw std::runtime_error("No more metadata left (peek).");
    return metadata[pos];
}

std::unique_ptr<TorrentFile> bencode::Parser::parse()
{
    if (get() != 'd')
        throw std::runtime_error(
            "The content of the .torrent file must be a bencoded dictionary.");

    while (peek() != 'e')
    {
        std::shared_ptr<BString> bs = parse_byte_string();

        size_t start = pos;
        std::shared_ptr<BType> bt = parse_bencoding_type();
        size_t end = pos;
        metadata_dict->content[bs->content] = bt;

        if (bs->content == "info")
        {
            info_dict_raw_bytes.assign(metadata.begin() + start,
                                       metadata.begin() + end);
        }
    }

    if (get() != 'e')
        throw std::runtime_error("Dictionary not properly terminated.");

    return std::make_unique<TorrentFile>(metadata_dict, info_dict_raw_bytes);
}

std::shared_ptr<BString> bencode::Parser::parse_byte_string()
{
    size_t i = pos;
    for (; i < metadata.size(); i++)
    {
        if (metadata[i] == ':')
            break;
        if (!std::isdigit(metadata[i]))
            throw std::runtime_error(
                "Error parsing the length of the string at position " +
                std::to_string(i) + '.');
    }

    std::string numStr(metadata.begin() + pos, metadata.begin() + i);
    size_t length = std::stoul(numStr);
    pos = i;

    if (get() != ':')
        throw std::runtime_error("Missing \":\" at pos " + std::to_string(pos) + '.');

    auto b = std::make_shared<BString>();
    if (pos + length > metadata.size())
        throw std::runtime_error(
            "String at the end of the file wasn't finished at position " +
            std::to_string(pos) + '.');

    b->content =
        std::string(metadata.begin() + pos, metadata.begin() + pos + length);
    pos = pos + length;
    return b;
}

std::shared_ptr<BType> bencode::Parser::parse_bencoding_type()
{
    char nextChar = peek();
    switch (nextChar)
    {
        case 'i':
            return parse_integer();
        case 'l':
            return parse_list();
        case 'd':
            return parse_dictionary();
    }

    if (std::isdigit(nextChar))
        return parse_byte_string();

    throw std::runtime_error(
        "Unable to parse type at position: " + std::to_string(pos) + '.');
}

std::shared_ptr<BInteger> bencode::Parser::parse_integer()
{
    if (get() != 'i')
        throw std::runtime_error("Integer must start with 'i'");

    size_t start = pos;
    while (peek() != 'e')
    {
        if (!std::isdigit(peek()) && peek() != '-')
        {
            throw std::runtime_error(
                "Invalid character in integer at position " +
                std::to_string(pos));
        }
        get();
    }

    std::string numStr(metadata.begin() + start, metadata.begin() + pos);
    get();  // consume ending 'e'

    auto bint = std::make_shared<BInteger>();
    bint->value = std::stoll(numStr);
    return bint;
}

std::shared_ptr<BList> bencode::Parser::parse_list()
{
    if (get() != 'l')
        throw std::runtime_error("List must start with 'l'");

    auto blist = std::make_shared<BList>();
    while (peek() != 'e')
    {
        blist->content.push_back(parse_bencoding_type());
    }

    if (get() != 'e')
        throw std::runtime_error("List not properly terminated.");

    return blist;
}

std::shared_ptr<BDict> bencode::Parser::parse_dictionary()
{
    if (get() != 'd')
        throw std::runtime_error("Dictionary must start with 'd'");

    auto bdict = std::make_shared<BDict>();
    while (peek() != 'e')
    {
        auto key = parse_byte_string();
        auto value = parse_bencoding_type();

        bdict->content[key->content] = std::move(value);
    }

    if (get() != 'e')  // consume ending 'e'
        throw std::runtime_error("Dictionary not properly terminated.");

    return bdict;
}