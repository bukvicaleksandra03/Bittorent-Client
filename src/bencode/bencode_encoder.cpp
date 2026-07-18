#include "bencode/bencode_encoder.h"

namespace bencode
{

std::string integer(int64_t v)
{
    return "i" + std::to_string(v) + "e";
}

std::string string(const std::string& s)
{
    return std::to_string(s.size()) + ":" + s;
}

std::string list(const std::vector<std::string>& items)
{
    std::string out = "l";
    for (const auto& item : items)
        out += item;
    out += "e";
    return out;
}

std::string dict(const std::map<std::string, std::string>& entries)
{
    // std::map is already sorted by key, which satisfies BEP 3.
    std::string out = "d";
    for (const auto& [k, v] : entries)
    {
        out += string(k);
        out += v;
    }
    out += "e";
    return out;
}

}  // namespace bencode
