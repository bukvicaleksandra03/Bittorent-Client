#pragma once

#include <chrono>
#include <mutex>
#include <string>

namespace dht
{

// Issues and verifies BEP-5 get_peers tokens for inbound announce_peer requests.
// Rotates the active secret every 5 minutes; the previous secret remains valid
// so tokens are accepted for up to ~10 minutes.
class TokenSecretRotator
{
   public:
    TokenSecretRotator();

    std::string current_token() const;
    bool verify_token(const std::string& token) const;
    void rotate_if_needed(std::chrono::steady_clock::time_point now =
                              std::chrono::steady_clock::now());

   private:
    mutable std::mutex mutex_;
    std::string current_token_;
    std::string prev_token_;
    std::chrono::steady_clock::time_point last_rotation_;
};

}  // namespace dht
