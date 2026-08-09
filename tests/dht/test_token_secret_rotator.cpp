#include "dht/token_secret_rotator.h"

#include <gtest/gtest.h>

namespace dht
{
namespace
{

TEST(TokenSecretRotatorTest, TokenIsStableForSameIp)
{
    TokenSecretRotator rotator;
    const std::string ip = "192.168.1.10";

    const std::string first = rotator.issue_token(ip);
    const std::string second = rotator.issue_token(ip);

    ASSERT_EQ(20u, first.size());
    EXPECT_EQ(first, second);
}

TEST(TokenSecretRotatorTest, DifferentIpsGetDifferentTokens)
{
    TokenSecretRotator rotator;

    const std::string token_a = rotator.issue_token("10.0.0.1");
    const std::string token_b = rotator.issue_token("10.0.0.2");

    EXPECT_NE(token_a, token_b);
}

TEST(TokenSecretRotatorTest, VerifyAcceptsIssuedTokenForSameIp)
{
    TokenSecretRotator rotator;
    const std::string ip = "127.0.0.1";
    const std::string token = rotator.issue_token(ip);

    EXPECT_TRUE(rotator.verify_token(token, ip));
}

TEST(TokenSecretRotatorTest, VerifyRejectsWrongIp)
{
    TokenSecretRotator rotator;
    const std::string token = rotator.issue_token("127.0.0.1");

    EXPECT_FALSE(rotator.verify_token(token, "127.0.0.2"));
}

TEST(TokenSecretRotatorTest, VerifyRejectsGarbageToken)
{
    TokenSecretRotator rotator;
    EXPECT_FALSE(rotator.verify_token(std::string(20, '\xAB'), "127.0.0.1"));
}

TEST(TokenSecretRotatorTest, PreviousSecretAcceptedAfterRotation)
{
    TokenSecretRotator rotator(std::chrono::milliseconds(0));
    const std::string ip = "203.0.113.5";

    const std::string old_token = rotator.issue_token(ip);
    ASSERT_TRUE(rotator.verify_token(old_token, ip));

    rotator.rotate_if_needed();
    const std::string new_token = rotator.issue_token(ip);
    EXPECT_NE(old_token, new_token);

    EXPECT_TRUE(rotator.verify_token(old_token, ip));
    EXPECT_TRUE(rotator.verify_token(new_token, ip));
}

TEST(TokenSecretRotatorTest, RotationIntervalPreventsImmediateRotate)
{
    TokenSecretRotator rotator(std::chrono::minutes(5));
    const std::string ip = "198.51.100.7";
    const std::string before = rotator.issue_token(ip);

    rotator.rotate_if_needed();
    const std::string after = rotator.issue_token(ip);

    EXPECT_EQ(before, after);
}

}  // namespace
}  // namespace dht
