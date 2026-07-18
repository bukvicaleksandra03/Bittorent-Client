#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Lightweight bencode serialiser used by the DHT/KRPC layer.
// Produces canonical bencoded byte strings suitable for UDP transmission.
//
// Usage examples:
//   bencode::integer(42)             -> "i42e"
//   bencode::string("hello")         -> "5:hello"
//   bencode::list({"a","bb"})        -> "l1:a2:bbe"
//   bencode::dict({{"k","v"}})       -> "d1:k1:ve"

namespace bencode
{

// Encode a 64-bit signed integer.
std::string integer(int64_t v);

// Encode a raw byte string (may contain NUL bytes).
std::string string(const std::string& s);

// Encode a list of already-encoded bencode values.
// The caller must pre-encode each element.
std::string list(const std::vector<std::string>& items);

// Encode a dictionary of already-encoded bencode values.
// Keys are raw strings (not pre-encoded); values must be pre-encoded.
// The dictionary is sorted by key before encoding (BEP 3 requirement).
std::string dict(const std::map<std::string, std::string>& entries);

}  // namespace bencode
