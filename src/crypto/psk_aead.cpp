#include <array>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <sodium.h>

#include "crypto/psk_aead.hpp"

namespace aead
{

static_assert(aead::KEY_SIZE == crypto_aead_xchacha20poly1305_ietf_KEYBYTES, "key size mismatch");
static_assert(aead::NONCE_SIZE == crypto_aead_xchacha20poly1305_ietf_NPUBBYTES,
              "nonce size mismatch");
static_assert(aead::TAG_SIZE == crypto_aead_xchacha20poly1305_ietf_ABYTES, "tag size mismatch");

static bool ensure_sodium_init()
{
    static int ok = (sodium_init() >= 0);  // -1 means failed
    return ok;
}

std::optional<SodiumPskAead> SodiumPskAead::CheckAndInitFromEnv(const char *env_var)
{
    if (!env_var)
        return std::nullopt;

    ensure_sodium_init();
    const char *s = getenv(env_var);
    if (!s)
        return std::nullopt;
    std::array<std::uint8_t, KEY_SIZE> key;
    std::size_t                        out_len = 0;
    if (sodium_hex2bin(key.data(), key.size(), s, strlen(s), nullptr, &out_len, nullptr) != 0 ||
        out_len != key.size())
    {
        return std::nullopt;
    }
    // success, init instance
    return SodiumPskAead{key};
}

bool SodiumPskAead::seal(std::string_view           msg,
                         const std::uint8_t        *ad,
                         std::size_t                adlen,
                         std::vector<std::uint8_t> &out)
{
    ensure_sodium_init();

    // settings from libsodium doc
    // output format = [NONCE | c] (c = mlen + TAG_SIZE)
    const std::size_t mlen = msg.size();
    out.resize(NONCE_SIZE + mlen + TAG_SIZE);

    unsigned char       *npub = out.data();               // [0...NONCE_SIZE)
    unsigned char       *c    = out.data() + NONCE_SIZE;  // [NONCE_SIZE...)
    unsigned long long   clen = 0;
    const unsigned char *m    = reinterpret_cast<const unsigned char *>(msg.data());

    randombytes_buf(out.data(), NONCE_SIZE);
    const unsigned char *k = have_session_ ? key_tx_.data() : key_.data();
    const int rc = crypto_aead_xchacha20poly1305_ietf_encrypt(c, &clen, m, msg.size(), ad, adlen,
                                                              /*nsec=*/nullptr, npub, k);

    if (rc != 0)
    {
        return false;
    }
    out.resize(NONCE_SIZE + static_cast<std::size_t>(clen));
    return true;
}

bool SodiumPskAead::open(const std::vector<std::uint8_t> &in,
                         const std::uint8_t              *ad,
                         std::size_t                      adlen,
                         std::vector<std::uint8_t>       &out)
{
    ensure_sodium_init();
    // [npub (NONCE_SIZE)] [c (ciphertext || tag)]
    if (in.size() < NONCE_SIZE + TAG_SIZE)
        return false;

    const unsigned char     *npub = in.data();               // NONCE
    const unsigned char     *c    = in.data() + NONCE_SIZE;  // ciphertext || tag
    const unsigned long long clen = in.size() - NONCE_SIZE;
    unsigned long long       mlen = 0;
    // allocate size for plaintext
    out.resize(clen > TAG_SIZE ? clen - TAG_SIZE : 0);
    unsigned char *m = reinterpret_cast<unsigned char *>(out.data());

    // try key_rx first (if have session), then fallback to key_
    int rc = -1;
    if (have_session_)
    {
        rc = crypto_aead_xchacha20poly1305_ietf_decrypt(m, &mlen, /*nsec=*/nullptr, c, clen, ad,
                                                        adlen, npub, key_rx_.data());
    }
    if (rc != 0)
    {
        rc = crypto_aead_xchacha20poly1305_ietf_decrypt(m, &mlen, /*nsec=*/nullptr, c, clen, ad,
                                                        adlen, npub, key_.data());
    }
    if (rc != 0)
    {
        return false;
    }
    out.resize(mlen);
    return true;
}

bool SodiumPskAead::set_session(const SessionKeys *k)
{
    if (k == nullptr)
    {
        have_session_ = false;
        sodium_memzero(key_tx_.data(), key_tx_.size());
        sodium_memzero(key_rx_.data(), key_rx_.size());
        sodium_memzero(nonce_tx_.data(), nonce_tx_.size());
        sodium_memzero(nonce_rx_.data(), nonce_rx_.size());
        return true;
    }
    std::memcpy(key_tx_.data(), k->ke_c2p.data(), key_tx_.size());
    std::memcpy(key_rx_.data(), k->ke_p2c.data(), key_rx_.size());
    std::memcpy(nonce_tx_.data(), k->n24_c2p.data(), nonce_tx_.size());
    std::memcpy(nonce_rx_.data(), k->n24_p2c.data(), nonce_rx_.size());
    have_session_ = true;
    return true;
}

void SodiumPskAead::nonce_inc_24(std::uint8_t n[NONCE_SIZE])
{
    for (int i = NONCE_SIZE - 1; i >= 0; --i)
    {
        if (++n[i] != 0)
            break;
    }
}

}  // namespace aead
