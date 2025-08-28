// tests/frag_test.cpp
#include <cstdint>
#include <gtest/gtest.h>
#include <vector>
#include "proto/frag.hpp"

using namespace frag;

static std::vector<std::uint8_t> gen_bytes(std::size_t n)
{
    std::vector<std::uint8_t> v(n);
    for (std::size_t i = 0; i < n; ++i)
        v[i] = static_cast<std::uint8_t>(i & 0xFF);
    return v;
}

TEST(Frag, HeaderPackUnpack)
{
    Header h{};
    h.ver    = 1;
    h.flags  = 0;
    h.msg_id = 42;
    h.seq    = 3;
    h.total  = 7;
    h.len    = 55;

    std::uint8_t buf[HDR_SIZE];
    ASSERT_TRUE(pack_header(h, buf));

    Header h2{};
    ASSERT_TRUE(unpack_header(buf, h2));

    EXPECT_EQ(h2.ver, 1);
    EXPECT_EQ(h2.flags, 0);
    EXPECT_EQ(h2.msg_id, 42u);
    EXPECT_EQ(h2.seq, 3u);
    EXPECT_EQ(h2.total, 7u);
    EXPECT_EQ(h2.len, 55u);
}

TEST(Frag, SerializeParse_Roundtrip)
{
    Chunk c{};
    c.hdr.ver    = 1;
    c.hdr.flags  = FLAG_FINAL;
    c.hdr.msg_id = 99;
    c.hdr.seq    = 0;
    c.hdr.total  = 2;
    c.payload    = std::vector<std::uint8_t>{'h', 'e', 'l', 'l', 'o'};
    c.hdr.len    = static_cast<std::uint16_t>(c.payload.size());

    auto frame = serialize(c);
    ASSERT_FALSE(frame.empty());

    auto parsed = parse(frame);
    ASSERT_TRUE(parsed.has_value());
    const Chunk &d = *parsed;

    EXPECT_EQ(d.hdr.ver, 1);
    EXPECT_EQ(d.hdr.flags, FLAG_FINAL);
    EXPECT_EQ(d.hdr.msg_id, 99u);
    EXPECT_EQ(d.hdr.seq, 0u);
    EXPECT_EQ(d.hdr.total, 2u);
    EXPECT_EQ(d.hdr.len, c.hdr.len);
    EXPECT_EQ(d.payload, c.payload);
}

TEST(Frag, SerializeParse_ZeroLen)
{
    Chunk c{};
    c.hdr.ver    = 1;
    c.hdr.flags  = 0;
    c.hdr.msg_id = 7;
    c.hdr.seq    = 0;
    c.hdr.total  = 1;
    c.hdr.len    = 0;

    auto frame = serialize(c);
    ASSERT_FALSE(frame.empty());

    auto parsed = parse(frame);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->hdr.len, 0u);
    EXPECT_TRUE(parsed->payload.empty());
}

TEST(Frag, MakeChunks_EmptyPayload)
{
    auto chunks = make_chunks(123, {}, 100);
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks[0].hdr.msg_id, 123u);
    EXPECT_EQ(chunks[0].hdr.seq, 0u);
    EXPECT_EQ(chunks[0].hdr.total, 1u);
    EXPECT_EQ(chunks[0].hdr.len, 0u);
    EXPECT_TRUE(chunks[0].payload.empty());
}

TEST(Frag, MakeChunks_ExactMultiple)
{
    auto bytes  = gen_bytes(300);
    auto chunks = make_chunks(1, bytes, 100);
    ASSERT_EQ(chunks.size(), 3u);
    for (std::size_t i = 0; i < chunks.size(); ++i)
    {
        EXPECT_EQ(chunks[i].hdr.seq, i);
        EXPECT_EQ(chunks[i].hdr.total, 3u);
        EXPECT_EQ(chunks[i].hdr.len, 100u);
    }
    // Reassemble manually and compare
    std::vector<std::uint8_t> merged;
    for (auto &c : chunks)
        merged.insert(merged.end(), c.payload.begin(), c.payload.end());
    EXPECT_EQ(merged, bytes);
}

TEST(Frag, MakeChunks_NonMultiple)
{
    auto bytes  = gen_bytes(230);
    auto chunks = make_chunks(2, bytes, 100);
    ASSERT_EQ(chunks.size(), 3u);
    EXPECT_EQ(chunks[0].hdr.len, 100u);
    EXPECT_EQ(chunks[1].hdr.len, 100u);
    EXPECT_EQ(chunks[2].hdr.len, 30u);
}

TEST(Frag, Reassembler_OutOfOrder_WithDup)
{
    auto bytes  = gen_bytes(230);
    auto chunks = make_chunks(77, bytes, 100);
    ASSERT_EQ(chunks.size(), 3u);

    Reassembler r;

    // feed seq0, then duplicate seq0, then seq2, then seq1 — only the last should complete
    auto r0 = r.feed(chunks[0]);
    // expect nullopt
    EXPECT_FALSE(r0.has_value());
    auto rdup = r.feed(chunks[0]);
    // duplicate
    EXPECT_FALSE(rdup.has_value());
    auto r2 = r.feed(chunks[2]);
    // still incomplete
    EXPECT_FALSE(r2.has_value());
    auto r1 = r.feed(chunks[1]);
    // completes
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(*r1, bytes);
}

TEST(Frag, Parse_RejectBadSize)
{
    Chunk c{};
    c.hdr.ver    = 1;
    c.hdr.flags  = 0;
    c.hdr.msg_id = 5;
    c.hdr.seq    = 0;
    c.hdr.total  = 1;
    c.payload    = gen_bytes(16);
    c.hdr.len    = static_cast<std::uint16_t>(c.payload.size());

    auto frame = serialize(c);
    ASSERT_FALSE(frame.empty());
    frame.pop_back();  // corrupt: size no longer equals HDR_SIZE + len

    auto bad = parse(frame);
    EXPECT_FALSE(bad.has_value());
}