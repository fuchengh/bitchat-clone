#include <array>
#include <cstring>
#include <gtest/gtest.h>
#include <string_view>
#include <vector>

#include "crypto/psk_aead.hpp"

using aead::SessionKeys;
using aead::SodiumPskAead;

namespace
{
// same AAD as daemon
constexpr std::uint8_t AAD[] = {'B', 'C', '1'};

struct EnvGuard
{
    std::string key;
    std::string old;
    bool        had{false};
    EnvGuard(const char *k, const char *v) : key(k)
    {
        const char *p = std::getenv(k);
        if (p)
        {
            had = true;
            old = p;
        }
        setenv(k, v, 1);
    }
    ~EnvGuard()
    {
        if (had)
            setenv(key.c_str(), old.c_str(), 1);
        else
            unsetenv(key.c_str());
    }
};
}  // namespace

TEST(AEAD_Session, Roundtrip)
{
    EnvGuard g("BITCHAT_PSK", "0000000000000000000000000000000000000000000000000000000000000000");

    auto c_opt = SodiumPskAead::CheckAndInitFromEnv("BITCHAT_PSK");
    auto p_opt = SodiumPskAead::CheckAndInitFromEnv("BITCHAT_PSK");
    ASSERT_TRUE(c_opt.has_value());
    ASSERT_TRUE(p_opt.has_value());
    SodiumPskAead central = *c_opt;
    SodiumPskAead periph  = *p_opt;

    SessionKeys k{};
    k.ke_c2p.fill(0x11);  // central->peripheral
    k.ke_p2c.fill(0x22);  // peripheral->central
    k.n24_c2p.fill(0xA1);
    k.n24_p2c.fill(0xB2);

    ASSERT_TRUE(central.set_session(&k));
    SessionKeys kp = k;
    std::swap(kp.ke_c2p, kp.ke_p2c);
    std::swap(kp.n24_c2p, kp.n24_p2c);
    ASSERT_TRUE(periph.set_session(&kp));

    // C->P
    const std::string_view    msg1 = "hello session c2p";
    std::vector<std::uint8_t> ciph1, plain1;
    ASSERT_TRUE(central.seal(msg1, AAD, sizeof(AAD), ciph1));
    ASSERT_TRUE(periph.open(ciph1, AAD, sizeof(AAD), plain1));
    ASSERT_EQ(std::string(plain1.begin(), plain1.end()), msg1);

    // P->C
    const std::string_view    msg2 = "hello session p2c";
    std::vector<std::uint8_t> ciph2, plain2;
    ASSERT_TRUE(periph.seal(msg2, AAD, sizeof(AAD), ciph2));
    ASSERT_TRUE(central.open(ciph2, AAD, sizeof(AAD), plain2));
    ASSERT_EQ(std::string(plain2.begin(), plain2.end()), msg2);
}

TEST(AEAD_Session, FallbackToEnvIfSenderNoSession)
{
    EnvGuard g("BITCHAT_PSK", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");

    auto s1o = SodiumPskAead::CheckAndInitFromEnv("BITCHAT_PSK");  // sender
    auto s2o = SodiumPskAead::CheckAndInitFromEnv("BITCHAT_PSK");  // receiver
    ASSERT_TRUE(s1o.has_value());
    ASSERT_TRUE(s2o.has_value());
    SodiumPskAead sender = *s1o;
    SodiumPskAead recv   = *s2o;

    // Receiver installs a wrong session (should not match env key)
    SessionKeys wrong{};
    wrong.ke_c2p.fill(0x33);
    wrong.ke_p2c.fill(0x44);
    wrong.n24_c2p.fill(0x55);
    wrong.n24_p2c.fill(0x66);
    ASSERT_TRUE(recv.set_session(&wrong));

    // Sender DOES NOT set a session → uses env key to seal.
    const std::string_view    msg = "env-key ciphertext";
    std::vector<std::uint8_t> ciph, plain;
    ASSERT_TRUE(sender.seal(msg, AAD, sizeof(AAD), ciph));

    // Receiver: try session key first (will fail), then env key (should succeed)
    ASSERT_TRUE(recv.open(ciph, AAD, sizeof(AAD), plain));
    ASSERT_EQ(std::string(plain.begin(), plain.end()), msg);
}

TEST(AEAD_Sodium, PSK_Mismatch)
{
    const char *key1 = "1111111111111111111111111111111111111111111111111111111111111111";
    const char *key2 = "2222222222222222222222222222222222222222222222222222222222222222";

    // create AEAD instance with key1
    EnvGuard g1("BITCHAT_PSK", key1);
    auto     a1_opt = SodiumPskAead::CheckAndInitFromEnv("BITCHAT_PSK");
    ASSERT_TRUE(a1_opt.has_value());
    SodiumPskAead a1 = *a1_opt;

    const std::string         msg = "mismatch should fail";
    std::vector<std::uint8_t> sealed;
    ASSERT_TRUE(a1.seal(msg, AAD, sizeof(AAD), sealed));
    ASSERT_FALSE(sealed.empty());

    // switch to key2, should fail while decrypting
    EnvGuard g2("BITCHAT_PSK", key2);
    auto     a2_opt = SodiumPskAead::CheckAndInitFromEnv("BITCHAT_PSK");
    ASSERT_TRUE(a2_opt.has_value());
    SodiumPskAead a2 = *a2_opt;

    std::vector<std::uint8_t> plain;
    EXPECT_FALSE(a2.open(sealed, AAD, sizeof(AAD), plain));

    // sanity：check again with correct key (key1)
    plain.clear();
    EXPECT_TRUE(a1.open(sealed, AAD, sizeof(AAD), plain));
    EXPECT_EQ(std::string(plain.begin(), plain.end()), msg);
}
