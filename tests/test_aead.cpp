#include <cstdlib>
#include <gtest/gtest.h>
#include <string>
#include <vector>
#include "crypto/psk_aead.hpp"

using namespace aead;

namespace
{
struct EnvGuard
{
    std::string var;
    std::string old;
    bool        had = false;

    EnvGuard(const char *v, const char *val) : var(v)
    {
        if (const char *p = std::getenv(var.c_str()))
        {
            had = true;
            old = p;
        }
        ::setenv(var.c_str(), val, 1);
    }

    ~EnvGuard()
    {
        if (had)
            ::setenv(var.c_str(), old.c_str(), 1);
        else
            ::unsetenv(var.c_str());
    }
};
}  // namespace

TEST(AEAD_Sodium, FromEnv_InvalidHex)
{
    EnvGuard g("BITCHAT_PSK", "not-hex");
    auto     s = SodiumPskAead::CheckAndInitFromEnv("BITCHAT_PSK");
    EXPECT_FALSE(s.has_value());
}

TEST(AEAD_Sodium, Roundtrip_Hello)
{
    // 32-byte key: all 0x11 -> 64 hex '11...'
    const char *keyhex = "1111111111111111111111111111111111111111111111111111111111111111";
    EnvGuard    g("BITCHAT_PSK", keyhex);

    auto s = SodiumPskAead::CheckAndInitFromEnv("BITCHAT_PSK");
    ASSERT_TRUE(s.has_value());
    SodiumPskAead aead = *s;

    const std::string         msg = "hello";
    std::vector<std::uint8_t> ct, pt;

    ASSERT_TRUE(aead.seal(msg, nullptr, 0, ct));
    // out = NONCE || (ciphertext||tag)
    ASSERT_GE(ct.size(), NONCE_SIZE + TAG_SIZE);
    EXPECT_EQ(ct.size(), NONCE_SIZE + msg.size() + TAG_SIZE);
    // open successfully
    ASSERT_TRUE(aead.open(ct, nullptr, 0, pt));
    EXPECT_EQ(std::string(pt.begin(), pt.end()), msg);

    ct.back() ^= 0x01;
    EXPECT_FALSE(aead.open(ct, nullptr, 0, pt));
}

TEST(AEAD_Sodium, Roundtrip_ZeroLen)
{
    const char *keyhex = "2222222222222222222222222222222222222222222222222222222222222222";
    EnvGuard    g("BITCHAT_PSK", keyhex);

    auto s = SodiumPskAead::CheckAndInitFromEnv("BITCHAT_PSK");
    ASSERT_TRUE(s.has_value());
    SodiumPskAead aead = *s;

    std::vector<std::uint8_t> ct, pt;
    ASSERT_TRUE(aead.seal(std::string_view{}, nullptr, 0, ct));
    EXPECT_EQ(ct.size(), NONCE_SIZE + TAG_SIZE);

    ASSERT_TRUE(aead.open(ct, nullptr, 0, pt));
    EXPECT_TRUE(pt.empty());
}

TEST(AEAD_Sodium, AAD_Mismatch_Fails)
{
    const char *keyhex = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    EnvGuard    g("BITCHAT_PSK", keyhex);

    auto s = SodiumPskAead::CheckAndInitFromEnv("BITCHAT_PSK");
    ASSERT_TRUE(s.has_value());
    SodiumPskAead aead = *s;

    const std::string msg     = "with aad";
    const std::string aad_ok  = "hdr";
    const std::string aad_bad = "HDR";

    std::vector<std::uint8_t> ct, pt;

    ASSERT_TRUE(aead.seal(msg, reinterpret_cast<const std::uint8_t *>(aad_ok.data()),
                          aad_ok.size(), ct));

    // correct AAD -> ok
    ASSERT_TRUE(aead.open(ct, reinterpret_cast<const std::uint8_t *>(aad_ok.data()), aad_ok.size(),
                          pt));
    EXPECT_EQ(std::string(pt.begin(), pt.end()), msg);

    // wrong AAD -> fail
    EXPECT_FALSE(aead.open(ct, reinterpret_cast<const std::uint8_t *>(aad_bad.data()),
                           aad_bad.size(), pt));
}