#include <arpa/inet.h>  // htonl, htons, ntohl, ntohs
#include <cstdint>
#include <cstring>

#include "proto/frag.hpp"
#include "util/log.hpp"

namespace frag
{

std::vector<Chunk> make_chunks(std::uint32_t                    msg_id,
                               const std::vector<std::uint8_t> &payload,
                               std::size_t                      mtu_payload)
{
    // also do validation here...
    if (mtu_payload < 1 || mtu_payload > MAX_PAYLOAD)
    {
        LOG_ERROR("make_chunks: invalid mtu_payload (%zu)", mtu_payload);
        return {};
    }
    std::vector<Chunk> out;
    if (payload.empty())
    {
        Chunk c;
        c.hdr.ver    = PROTO_VER;
        c.hdr.flags  = FLAG_FINAL;
        c.hdr.msg_id = msg_id;
        c.hdr.seq    = 0;
        c.hdr.total  = 1;
        c.hdr.len    = 0;
        out.push_back(std::move(c));
        return out;
    }

    const std::size_t num_chunks = (payload.size() + mtu_payload - 1) / mtu_payload;
    if (num_chunks > UINT16_MAX)
    {
        LOG_ERROR("make_chunks: payload too large (%zu bytes, needs %zu chunks)", payload.size(),
                  num_chunks);
        return {};
    }

    for (std::size_t i = 0; i < num_chunks; i++)
    {
        std::size_t start    = i * mtu_payload;
        std::size_t take     = std::min(mtu_payload, payload.size() - start);
        bool        is_final = (i + 1 == num_chunks);
        Chunk       c;
        c.hdr.ver    = PROTO_VER;
        c.hdr.flags  = is_final ? FLAG_FINAL : 0;
        c.hdr.msg_id = msg_id;
        c.hdr.seq    = static_cast<std::uint16_t>(i);
        c.hdr.total  = static_cast<std::uint16_t>(num_chunks);
        c.hdr.len    = static_cast<std::uint16_t>(take);
        if (take)
            c.payload.insert(c.payload.end(), payload.begin() + start,
                             payload.begin() + start + take);
        out.push_back(std::move(c));
    }

    return out;
}

std::vector<std::uint8_t> serialize(const Chunk &c)
{
    if (c.payload.size() != c.hdr.len)
    {
        LOG_ERROR("serialize: payload size mismatch (%zu != %u)", c.payload.size(),
                  static_cast<unsigned>(c.hdr.len));
        return {};
    }

    std::vector<std::uint8_t> out(HDR_SIZE + c.payload.size());

    if (!pack_header(c.hdr, out.data()))
    {
        LOG_ERROR("serialize: invalid header");
        return {};
    }

    if (!c.payload.empty())
    {
        std::memcpy(out.data() + HDR_SIZE, c.payload.data(), c.payload.size());
    }
    return out;
}

std::optional<Chunk> parse(const std::vector<std::uint8_t> &frame)
{
    // Parse: to make sure the received frame is valid, then extract the header and payload to our reassembler (as Chunk)
    Header h{};
    Chunk  c;
    if (frame.size() < HDR_SIZE)
    {
        LOG_ERROR("parse: frame too short! (%zu)", frame.size());
        return std::nullopt;
    }
    // unpack header
    if (!unpack_header(frame.data(), h))
    {
        LOG_ERROR("parse: invalid header, failed to unpack");
        return std::nullopt;
    }

    const std::size_t expected = HDR_SIZE + static_cast<std::size_t>(h.len);
    if (frame.size() != expected)
    {
        LOG_ERROR("parse: size mismatch (got %zu, expect %zu)", frame.size(), expected);
        return std::nullopt;
    }
    // copy out
    c.hdr = h;
    if (h.len)
    {
        c.payload.assign(frame.begin() + HDR_SIZE, frame.end());
    }
    return c;
}

bool pack_header(const Header &in, std::uint8_t out[HDR_SIZE])
{
    // validate fields before packing
    if (in.ver != PROTO_VER)
        return false;
    if (in.total == 0)
        return false;
    if (in.seq >= in.total)
        return false;
    if (in.len > MAX_PAYLOAD)
        return false;

    out[0] = in.ver;
    out[1] = in.flags;

    std::uint32_t msg_id_be = htonl(in.msg_id);
    std::memcpy(out + 2, &msg_id_be, sizeof msg_id_be);

    std::uint16_t seq_be = htons(in.seq);
    std::memcpy(out + 6, &seq_be, sizeof seq_be);

    std::uint16_t total_be = htons(in.total);
    std::memcpy(out + 8, &total_be, sizeof total_be);

    std::uint16_t len_be = htons(in.len);
    std::memcpy(out + 10, &len_be, sizeof len_be);

    return true;
}

bool unpack_header(const std::uint8_t in[HDR_SIZE], Header &out)
{
    out.ver   = in[0];
    out.flags = in[1];

    std::uint32_t msg_id_be;
    std::memcpy(&msg_id_be, in + 2, sizeof msg_id_be);
    out.msg_id = ntohl(msg_id_be);

    std::uint16_t seq_be;
    std::memcpy(&seq_be, in + 6, sizeof seq_be);
    out.seq = ntohs(seq_be);

    std::uint16_t total_be;
    std::memcpy(&total_be, in + 8, sizeof total_be);
    out.total = ntohs(total_be);

    std::uint16_t len_be;
    std::memcpy(&len_be, in + 10, sizeof len_be);
    out.len = ntohs(len_be);

    // validate after parsing
    if (out.ver != PROTO_VER)
        return false;
    if (out.total == 0)
        return false;
    if (out.seq >= out.total)
        return false;
    if (out.len > MAX_PAYLOAD)
        return false;

    return true;
}

std::optional<std::vector<std::uint8_t>> Reassembler::feed(const Chunk &c)
{
    // make sure this is a valid chunk
    if (c.hdr.total == 0 || c.hdr.seq >= c.hdr.total || c.hdr.len != c.payload.size())
    {
        LOG_ERROR("Reassembler::feed: invalid chunk");
        return std::nullopt;
    }
    std::uint32_t msg_id = c.hdr.msg_id;
    State        &st     = map_[msg_id];
    if (st.total == 0 || st.total != c.hdr.total)
    {
        // new message or total changed
        st.total    = c.hdr.total;
        st.received = 0;
        st.bytes    = 0;
        st.parts.assign(st.total, {});
        st.have.assign(st.total, false);
    }
    const std::uint16_t seq = c.hdr.seq;
    // ignore duplicates
    if (!st.have[seq])
    {
        st.parts[seq] = c.payload;
        st.have[seq]  = true;
        st.received++;
        st.bytes += c.payload.size();
    }
    else
    {
        // nop
        LOG_DEBUG("Reassembler::feed: duplicate chunk (msg_id=%u, seq=%u)", msg_id, seq);
    }

    if (st.received < st.total)
        return std::nullopt;  // not done yet

    // reassemble
    std::vector<std::uint8_t> out;
    out.reserve(st.bytes);
    for (std::uint16_t i = 0; i < st.total; i++)
    {
        if (!st.have[i])
        {
            // should not happen
            map_.erase(msg_id);
            return std::nullopt;
        }
        const auto &part = st.parts[i];
        if (!part.empty())
            out.insert(out.end(), part.begin(), part.end());
    }
    // clear state
    map_.erase(msg_id);
    return out;
}

}  // namespace frag
