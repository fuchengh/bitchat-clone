#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace frag
{

// --- Protocol constants (match design doc) ---
inline constexpr std::uint8_t PROTO_VER    = 1;
inline constexpr std::uint8_t FLAG_FINAL   = 1 << 0;
inline constexpr std::uint8_t FLAG_RETRANS = 1 << 1;
inline constexpr std::size_t  HDR_SIZE     = 12;

// On-wire chunk header
struct Chunk
{
    std::uint8_t              ver{PROTO_VER};
    std::uint8_t              flags{0};
    std::uint32_t             msg_id{0};
    std::uint16_t             seq{0};
    std::uint16_t             total{0};
    std::uint16_t             len{0};
    std::vector<std::uint8_t> data;
};

// TODO: Implement these in src/proto/frag.cpp
std::vector<Chunk> make_chunks(std::uint32_t                    msg_id,
                               const std::vector<std::uint8_t> &payload,
                               std::size_t                      mtu_payload);

std::vector<std::uint8_t> serialize(const Chunk &c);
std::optional<Chunk>      parse(const std::vector<std::uint8_t> &frame);

class Reassembler
{
  public:
    // Feed one chunk; return full payload when complete.
    std::optional<std::vector<std::uint8_t>> feed(const Chunk &c);
    void                                     clear(std::uint32_t msg_id);

  private:
    // TODO: add state for tracking partial messages
};

// Optional helper for logging
std::string hex(const std::vector<std::uint8_t> &v);

}  // namespace frag
