#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace aead
{

// TODO: define interface only here, provide a stub
// implementation, then swap to libsodium XChaCha20-Poly1305 in the next step.
class PskAead
{
  public:
    bool set_key(const std::array<std::uint8_t, 32> &key);
    bool set_key_hex(std::string_view hex);

    std::vector<std::uint8_t> seal(std::uint64_t                 ctr,
                                   std::span<const std::uint8_t> plain,
                                   std::span<const std::uint8_t> ad = {});

    std::optional<std::vector<std::uint8_t>> open(std::uint64_t                 ctr,
                                                  std::span<const std::uint8_t> cipher_with_tag,
                                                  std::span<const std::uint8_t> ad = {});
};

}  // namespace aead
