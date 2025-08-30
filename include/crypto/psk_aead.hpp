#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <vector>

namespace aead
{

constexpr std::size_t KEY_SIZE   = 32;  // crypto_aead_xchacha20poly1305_ietf_KEYBYTES
constexpr std::size_t NONCE_SIZE = 24;  // crypto_aead_xchacha20poly1305_ietf_NPUBBYTES
constexpr std::size_t TAG_SIZE   = 16;  // crypto_aead_xchacha20poly1305_ietf_ABYTES

class PskAead
{
  public:
    virtual ~PskAead() = default;

    virtual bool seal(std::string_view           plaintext,
                      const std::uint8_t        *aad,
                      std::size_t                aad_len,
                      std::vector<std::uint8_t> &out) = 0;

    virtual bool open(const std::vector<std::uint8_t> &in,
                      const std::uint8_t              *aad,
                      std::size_t                      aad_len,
                      std::vector<std::uint8_t>       &out) = 0;
};

class NoopPskAead : public PskAead
{
  public:
    bool seal(std::string_view plaintext,
              const std::uint8_t * /*aad*/,
              std::size_t /*aad_len*/,
              std::vector<std::uint8_t> &out) override
    {
        // for noop, keep the same format as XChaCha20-Poly1305 so we don't need to change the tests
        // format is [NONCE (24 bytes)][CIPHERTEXT (len(plaintext) bytes)][TAG (16 bytes)]
        out.resize(NONCE_SIZE + plaintext.size() + TAG_SIZE);
        std::memset(out.data(), 0, NONCE_SIZE);
        if (!plaintext.empty())
            std::memcpy(out.data() + NONCE_SIZE, plaintext.data(), plaintext.size());
        std::memset(out.data() + NONCE_SIZE + plaintext.size(), 0, TAG_SIZE);
        return true;
    }

    bool open(const std::vector<std::uint8_t> &in,
              const std::uint8_t * /*aad*/,
              std::size_t /*aad_len*/,
              std::vector<std::uint8_t> &out) override
    {
        if (in.size() < NONCE_SIZE + TAG_SIZE)
            return false;
        // read the ciphertext part only
        out.assign(in.begin() + NONCE_SIZE, in.end() - TAG_SIZE);
        return true;
    }
};

// libsodium-based implementation
class SodiumPskAead : public PskAead
{
  public:
    SodiumPskAead(const std::array<std::uint8_t, KEY_SIZE> &key) : key_(key) {}

    bool seal(std::string_view           msg,
              const std::uint8_t        *aad,
              std::size_t                aad_len,
              std::vector<std::uint8_t> &out) override;

    bool open(const std::vector<std::uint8_t> &in,
              const std::uint8_t              *aad,
              std::size_t                      aad_len,
              std::vector<std::uint8_t>       &out) override;

    static std::optional<SodiumPskAead> CheckAndInitFromEnv(const char *env_var);

  private:
    std::array<std::uint8_t, KEY_SIZE> key_{};
};

}  // namespace aead
