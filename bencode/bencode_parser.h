#ifndef __BENCODE_PARSER_H_
#define __BENCODE_PARSER_H_

#include <string>
#include <vector>
#include <memory>
#include <map>
#include <cstdint>
#include <iostream>

struct BType {
    virtual ~BType() = default;
    virtual void print(int indent = 0) const = 0;
};

struct BInteger : public BType {
    int64_t value;

    void print(int indent = 0) const override {
        std::cout << std::string(indent, ' ') << value;
    }
};

struct BString : public BType {
    std::string content;

    void print(int indent = 0) const override {
        std::cout << std::string(indent, ' ') << '"' << content << '"';
    }
};

struct BList : public BType {
    std::vector<std::unique_ptr<BType>> content;

    void print(int indent = 0) const override {
        std::cout << std::string(indent, ' ') << "[\n";
        for (const auto& item : content) {
            item->print(indent + 2);
            std::cout << "\n";
        }
        std::cout << std::string(indent, ' ') << "]";
    }
};

struct BDict : public BType {
    std::map<std::string, std::unique_ptr<BType>> content;

    void print(int indent = 0) const override {
        std::cout << std::string(indent, ' ') << "{\n";
        for (const auto& [key, value] : content) {
            std::cout << std::string(indent + 2, ' ') << '"' << key << "\": ";
            std::cout << std::endl;
            value->print(indent + 4);
            std::cout << "\n";
        }
        std::cout << std::string(indent, ' ') << "}";
    }
};

class BencodeParser 
{
public:
    explicit BencodeParser(const std::string& path);

    void parse();

    void print() { dict.print(); }

private:
    int pos;
    std::vector<char> metadata;

    char get();
    char peek() const;

    std::unique_ptr<BString> parse_byte_string();

    std::unique_ptr<BType> parse_bencoding_type();

    std::unique_ptr<BInteger> parse_integer();

    std::unique_ptr<BList> parse_list();

    std::unique_ptr<BDict> parse_dictionary();

    BDict dict;
};

#endif __BENCODE_PARSER_H_