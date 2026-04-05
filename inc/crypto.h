#pragma once

#include <openssl/evp.h>

#include <array>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace crypto
{

constexpr size_t SHA1_DIGEST_LENGTH = 20;
using SHA1Hash = std::array<uint8_t, SHA1_DIGEST_LENGTH>;

SHA1Hash sha1(const std::vector<uint8_t>& data);
std::string to_hex(const SHA1Hash& hash);
std::string url_encode(const SHA1Hash& hash);

// ---------------------------------------------------------------------------

inline SHA1Hash sha1(const std::vector<uint8_t>& data)
{
    SHA1Hash hash{};

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx)
    {
        throw std::runtime_error("Failed to create EVP_MD_CTX");
    }

    if (EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr) != 1)
    {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("EVP_DigestInit_ex failed");
    }

    if (EVP_DigestUpdate(ctx, data.data(), data.size()) != 1)
    {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("EVP_DigestUpdate failed");
    }

    unsigned int len = 0;
    if (EVP_DigestFinal_ex(ctx, hash.data(), &len) != 1)
    {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("EVP_DigestFinal_ex failed");
    }

    EVP_MD_CTX_free(ctx);
    return hash;
}

inline std::string to_hex(const SHA1Hash& hash)
{
    std::ostringstream oss;
    for (uint8_t byte : hash)
    {
        oss << std::hex << std::setfill('0') << std::setw(2)
            << static_cast<int>(byte);
    }
    return oss.str();
}

inline std::string url_encode(const SHA1Hash& hash)
{
    std::ostringstream oss;
    for (uint8_t byte : hash)
    {
        if (std::isalnum(byte) || byte == '-' || byte == '_' || byte == '.' ||
            byte == '~')
        {
            oss << static_cast<char>(byte);
        }
        else
        {
            oss << '%' << std::hex << std::uppercase << std::setfill('0')
                << std::setw(2) << static_cast<int>(byte);
        }
    }
    return oss.str();
}

}  // namespace crypto
