#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ctrl
{
constexpr uint8_t MSG_CTRL_HELLO = 0x01;
constexpr uint8_t HELLO_VER      = 0x01;
constexpr uint8_t T_USER_ID      = 0x01;
constexpr uint8_t T_CAPS         = 0x02;
constexpr uint8_t T_NA32         = 0x12;

constexpr uint32_t CAP_AEAD_PSK_SUPPORTED = 1u << 0;

struct Hello
{
    std::string             user_id;
    bool                    has_caps{false};
    uint32_t                caps{0};
    bool                    has_na32{false};
    std::array<uint8_t, 32> na32{};
};

inline std::vector<uint8_t> encode_hello(std::string_view user, uint32_t caps, const uint8_t *na32)
{
    std::vector<uint8_t> out;
    out.reserve(2 + 2 + user.size() + 2 + 4 + 2 + 32);
    out.push_back(MSG_CTRL_HELLO);
    out.push_back(HELLO_VER);
    if (!user.empty())
    {
        out.push_back(T_USER_ID);
        out.push_back(0x00);
        out.push_back((uint8_t)user.size());
        out.insert(out.end(), user.begin(), user.end());
    }
    out.push_back(T_CAPS);
    out.push_back(0x00);
    out.push_back(0x04);
    uint32_t       le = caps;  // host assumed LE
    const uint8_t *p  = reinterpret_cast<const uint8_t *>(&le);
    out.insert(out.end(), p, p + 4);
    if (na32)
    {
        out.push_back(T_NA32);
        out.push_back(0x00);
        out.push_back(0x20);
        out.insert(out.end(), na32, na32 + 32);
    }
    return out;
}

inline bool parse_hello(const uint8_t *buf, size_t len, Hello &h)
{
    if (len < 2 || buf[0] != MSG_CTRL_HELLO || buf[1] != HELLO_VER)
        return false;
    size_t i = 2;
    while (i + 3 <= len)
    {
        uint8_t  t = buf[i++];
        uint16_t L = (uint16_t)buf[i++] << 8;
        L |= buf[i++];
        if (i + L > len)
            return false;  // malformed => return false
        const uint8_t *v = buf + i;
        switch (t)
        {
            case T_USER_ID:
                h.user_id.assign((const char *)v, (size_t)L);
                break;
            case T_CAPS:
                if (L != 4)
                    return false;
                h.has_caps = true;
                h.caps     = (uint32_t)v[0] | ((uint32_t)v[1] << 8) | ((uint32_t)v[2] << 16) |
                         ((uint32_t)v[3] << 24);
                break;
            case T_NA32:
                if (L != 32)
                    return false;
                h.has_na32 = true;
                std::copy(v, v + 32, h.na32.begin());
                break;
            default: // ignore TLV if unknown
                break;
        }
        i += L;
    }
    return true;
}
}  // namespace ctrl