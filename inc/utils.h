// utils.h
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <random>
#include <sstream>

namespace utils
{

bool is_power_of_two(uint64_t n);

using PeerId = std::array<uint8_t, 20>;
PeerId generate_peer_id();
std::string to_hex(const PeerId& peer_id);

}  // namespace utils