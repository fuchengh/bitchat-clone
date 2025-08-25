#include "proto/frag.hpp"
// TODO: Implement make_chunks/serialize/parse/Reassembler.

namespace frag
{

std::vector<Chunk> make_chunks(std::uint32_t, const std::vector<std::uint8_t> &, std::size_t)
{
    return {};  // TODO
}

std::vector<std::uint8_t> serialize(const Chunk &)
{
    return {};
}  // TODO

std::optional<Chunk> parse(const std::vector<std::uint8_t> &)
{
    return std::nullopt;
}  // TODO

std::optional<std::vector<std::uint8_t>> Reassembler::feed(const Chunk &)
{
    return std::nullopt;  // TODO
}

void Reassembler::clear(std::uint32_t)
{ /* TODO */
}

std::string hex(const std::vector<std::uint8_t> &v)
{
    static const char *k = "0123456789abcdef";
    std::string        s;
    s.reserve(v.size() * 2);
    for (auto b : v)
    {
        s.push_back(k[(b >> 4) & 0xF]);
        s.push_back(k[b & 0xF]);
    }
    return s;
}

}  // namespace frag
