#include <cstdlib>
#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "app/chat_service.hpp"
#include "crypto/psk_aead.hpp"
#include "transport/loopback_transport.hpp"

struct CapturingNoopAead : public aead::NoopPskAead
{
    std::string last_plaintext;
    bool        open(const std::vector<std::uint8_t> &in,
                     const std::uint8_t              *aad,
                     std::size_t                      aad_len,
                     std::vector<std::uint8_t>       &out) override
    {
        const bool ok = aead::NoopPskAead::open(in, aad, aad_len, out);
        if (ok)
            last_plaintext.assign(reinterpret_cast<const char *>(out.data()), out.size());
        return ok;
    }
};

// A wrapper that forwards to AEAD (e.g., Sodium) and captures plaintext
struct CapturingAead : public aead::PskAead
{
    aead::PskAead &inner;
    std::string    last_plaintext;
    explicit CapturingAead(aead::PskAead &a) : inner(a) {}
    bool seal(std::string_view           plaintext,
              const std::uint8_t        *aad,
              std::size_t                aad_len,
              std::vector<std::uint8_t> &out) override
    {
        return inner.seal(plaintext, aad, aad_len, out);
    }
    bool open(const std::vector<std::uint8_t> &in,
              const std::uint8_t              *aad,
              std::size_t                      aad_len,
              std::vector<std::uint8_t>       &out) override
    {
        const bool ok = inner.open(in, aad, aad_len, out);
        if (ok)
            last_plaintext.assign(reinterpret_cast<const char *>(out.data()), out.size());
        return ok;
    }
};

TEST(ChatServiceLoopback, Noop_Short_Roundtrip)
{
    transport::LoopbackTransport t;
    CapturingNoopAead            aead;

    app::ChatService svc(t, aead, /*mtu_payload=*/100);
    ASSERT_TRUE(svc.start());

    const std::string msg = "hello, loopback!";
    ASSERT_TRUE(svc.send_text(msg));

    // Loopback is synchronous; after send_text, on_rx has run.
    EXPECT_EQ(aead.last_plaintext, msg);

    t.stop();
}

TEST(ChatServiceLoopback, Noop_Long_Roundtrip_Fragmented)
{
    transport::LoopbackTransport t;
    CapturingNoopAead            aead;
    // Force fragmentation with a small MTU
    app::ChatService svc(t, aead, /*mtu_payload=*/32);
    ASSERT_TRUE(svc.start());

    std::string msg(4096, 'X');  // > MTU, will be split into many chunks
    ASSERT_TRUE(svc.send_text(msg));
    EXPECT_EQ(aead.last_plaintext, msg);

    t.stop();
}

TEST(ChatServiceLoopback, Sodium_Long_Roundtrip_Fragmented)
{
    // Inject a fixed 32-byte (all-zero) key via env (64 hex chars)
    // This avoids depending on random and works deterministically in CI.
#if defined(_WIN32)
    _putenv("BITCHAT_PSK_HEX=0000000000000000000000000000000000000000000000000000000000000000");
#else
    setenv("BITCHAT_PSK_HEX", "0000000000000000000000000000000000000000000000000000000000000000",
           1);
#endif

    auto s = aead::SodiumPskAead::CheckAndInitFromEnv("BITCHAT_PSK_HEX");
    if (!s.has_value())
    {
        GTEST_SKIP() << "Sodium AEAD not available or key parse failed";
    }

    transport::LoopbackTransport t;
    aead::SodiumPskAead          sodium = *s;
    CapturingAead                cap(sodium);

    // Small MTU to exercise fragmentation of ciphertext+tag+nonce
    app::ChatService svc(t, cap, /*mtu_payload=*/48);
    ASSERT_TRUE(svc.start());

    std::string msg(2048, 'Z');
    ASSERT_TRUE(svc.send_text(msg));
    EXPECT_EQ(cap.last_plaintext, msg);

    t.stop();
}