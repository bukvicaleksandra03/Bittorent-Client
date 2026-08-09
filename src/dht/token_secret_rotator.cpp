#include "dht/token_secret_rotator.h"

#include <arpa/inet.h>
#include <openssl/rand.h>

#include <cstring>
#include <vector>

#include "crypto.h"

namespace dht
{

namespace
{

std::vector<uint8_t> ip_address_bytes(const std::string& ip)
{
    struct in_addr v4 {};
    if (::inet_pton(AF_INET, ip.c_str(), &v4) == 1)
    {
        std::vector<uint8_t> out(4);
        std::memcpy(out.data(), &v4, 4);
        return out;
    }

    struct in6_addr v6 {};
    if (::inet_pton(AF_INET6, ip.c_str(), &v6) == 1)
    {
        std::vector<uint8_t> out(16);
        std::memcpy(out.data(), &v6, 16);
        return out;
    }

    return {};
}

}  // namespace

TokenSecretRotator::TokenSecretRotator(
    std::chrono::milliseconds rotation_interval)
    : rotation_interval_(rotation_interval),
      current_secret_(random_secret()),
      last_rotation_(std::chrono::steady_clock::now())
{
}

std::string TokenSecretRotator::random_secret()
{
    std::string secret(20, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char*>(secret.data()),
                   static_cast<int>(secret.size())) != 1)
    {
        throw std::runtime_error("RAND_bytes failed");
    }
    return secret;
}

std::string TokenSecretRotator::compute_token(const std::string& secret,
                                              const std::string& ip)
{
    const std::vector<uint8_t> ip_bytes = ip_address_bytes(ip);
    std::vector<uint8_t> material;
    material.reserve(secret.size() + ip_bytes.size());
    material.insert(material.end(), secret.begin(), secret.end());
    material.insert(material.end(), ip_bytes.begin(), ip_bytes.end());

    const crypto::SHA1Hash hash = crypto::sha1(material);
    return std::string(reinterpret_cast<const char*>(hash.data()), hash.size());
}

void TokenSecretRotator::rotate_if_needed()
{
    const auto now = std::chrono::steady_clock::now();
    if (now - last_rotation_ < rotation_interval_)
        return;

    prev_secret_ = current_secret_;
    prev_secret_expires_ = now + PREV_SECRET_TTL;
    current_secret_ = random_secret();
    last_rotation_ = now;
}

std::string TokenSecretRotator::issue_token(const std::string& ip) const
{
    return compute_token(current_secret_, ip);
}

bool TokenSecretRotator::verify_token(const std::string& token,
                                      const std::string& ip) const
{
    if (token == compute_token(current_secret_, ip))
        return true;

    if (!prev_secret_.empty() &&
        std::chrono::steady_clock::now() < prev_secret_expires_ &&
        token == compute_token(prev_secret_, ip))
    {
        return true;
    }

    return false;
}

}  // namespace dht
