#include "crypto/psk_aead.hpp"
// TODO: Provide a placeholder implementation

namespace aead
{

bool PskAead::set_key(const std::array<std::uint8_t, 32> &)
{
    return true;
}
bool PskAead::set_key_hex(std::string_view)
{
    return true;
}

std::vector<std::uint8_t> PskAead::seal(std::uint64_t,
                                        std::span<const std::uint8_t> plain,
                                        std::span<const std::uint8_t>)
{
    return std::vector<std::uint8_t>(plain.begin(), plain.end());  // echo
}

std::optional<std::vector<std::uint8_t>> PskAead::open(std::uint64_t,
                                                       std::span<const std::uint8_t> c,
                                                       std::span<const std::uint8_t>)
{
    return std::vector<std::uint8_t>(c.begin(), c.end());  // echo
}

}  // namespace aead
