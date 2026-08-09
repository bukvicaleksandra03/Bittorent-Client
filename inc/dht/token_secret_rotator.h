#pragma once

#include <chrono>
#include <string>

namespace dht
{

// BEP-5 announce token issuer.
//
// Tokens are SHA1(current_secret + requester_ip_bytes). The secret rotates every
// five minutes; the previous secret remains valid for ten minutes so clients
// that received a token just before rotation can still announce.
class TokenSecretRotator
{
public:
    static constexpr auto DEFAULT_ROTATION_INTERVAL = std::chrono::minutes(5);
    static constexpr auto PREV_SECRET_TTL = std::chrono::minutes(10);

    explicit TokenSecretRotator(
        std::chrono::milliseconds rotation_interval = DEFAULT_ROTATION_INTERVAL);

    void rotate_if_needed();

    // Issue a token bound to the requester's IP (UDP source address).
    std::string issue_token(const std::string& ip) const;

    // Verify token against current or still-valid previous secret for that IP.
    bool verify_token(const std::string& token, const std::string& ip) const;

private:
    static std::string random_secret();
    static std::string compute_token(const std::string& secret,
                                     const std::string& ip);

    std::chrono::milliseconds rotation_interval_;
    std::string current_secret_;
    std::string prev_secret_;
    std::chrono::steady_clock::time_point last_rotation_;
    std::chrono::steady_clock::time_point prev_secret_expires_;
};

}  // namespace dht
