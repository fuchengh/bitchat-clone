#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

/*
TX:
app.send_text(plaintext)
  -> AEAD.seal(...) = nonce + ciphertext + tag
     -> make_chunks(msg_id, bytes, mtu)
        -> for each Chunk {hdr, data}:
             serialize(Chunk)  // serialized into frame [12B header][payload]
               -> transport.send(frame_bytes)

RX:
transport.on_rx(frame_bytes)
  -> parse(frame_bytes)  // validate and extract Chunk [hdr, payload]
      -> ok? reassembler.feed(Chunk)
            -> Complete ? AEAD.open(...) -> plaintext -> app
*/

namespace frag
{

// --- Protocol constants ---
inline constexpr std::uint8_t PROTO_VER    = 1;
inline constexpr std::uint8_t FLAG_FINAL   = 1 << 0;
inline constexpr std::uint8_t FLAG_RETRANS = 1 << 1;
inline constexpr std::size_t  HDR_SIZE     = 12;
inline constexpr std::size_t  MAX_PAYLOAD  = 100;  // 100 bytes per fragment

// On-wire chunk header
struct Header
{
    std::uint8_t  ver{PROTO_VER};  // 1B
    std::uint8_t  flags{0};        // 1B
    std::uint32_t msg_id{0};       // 4B
    std::uint16_t seq{0};          // 2B
    std::uint16_t total{0};        // 2B
    std::uint16_t len{0};          // 2B
};

struct Chunk
{
    Header                    hdr;
    std::vector<std::uint8_t> payload;
};
// TX
std::vector<Chunk>        make_chunks(std::uint32_t                    msg_id,
                                      const std::vector<std::uint8_t> &payload,
                                      std::size_t                      mtu_payload);
std::vector<std::uint8_t> serialize(const Chunk &c);
bool                      pack_header(const Header &in, uint8_t out[12]);
// RX
std::optional<Chunk>      parse(const std::vector<std::uint8_t> &frame);
bool                      unpack_header(const uint8_t in[12], Header &out);

class Reassembler
{
  public:
    // Feed one chunk, return complete payload if done
    std::optional<std::vector<std::uint8_t>> feed(const Chunk &c);
    void                                     clear(std::uint32_t msg_id) { map_.erase(msg_id); }

  private:
    struct State
    {
        std::uint16_t                          total    = 0;
        std::size_t                            received = 0;
        std::size_t                            bytes    = 0;
        std::vector<std::vector<std::uint8_t>> parts;  // size == total
        std::vector<bool>                      have;   // size == total
    };
    std::unordered_map<std::uint32_t, State> map_;
};

}  // namespace frag
