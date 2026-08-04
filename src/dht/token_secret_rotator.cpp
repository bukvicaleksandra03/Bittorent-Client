#include "dht/token_secret_rotator.h"

#include <iomanip>
#include <random>
#include <sstream>

namespace dht
{

namespace
{

static constexpr auto kRotateInterval = std::chrono::minutes(5);

std::string make_random_token_secret()
{
    static std::mt19937 gen(std::random_device{}());
    static std::uniform_int_distribution<unsigned> dist(0, 255);
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (int i = 0; i < 8; ++i)
        oss << std::setw(2) << dist(gen);
    return oss.str();
}

}  // namespace

TokenSecretRotator::TokenSecretRotator()
    : current_token_(make_random_token_secret()),
      prev_token_(make_random_token_secret()),
      last_rotation_(std::chrono::steady_clock::now())
{
}

std::string TokenSecretRotator::current_token() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return current_token_;
}

bool TokenSecretRotator::verify_token(const std::string& token) const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return token == current_token_ || token == prev_token_;
}

void TokenSecretRotator::rotate_if_needed(
    std::chrono::steady_clock::time_point now)
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (now - last_rotation_ >= kRotateInterval)
    {
        prev_token_ = current_token_;
        current_token_ = make_random_token_secret();
        last_rotation_ = now;
    }
}

}  // namespace dht
