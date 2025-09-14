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
constexpr std::size_t USER_ID_MAX            = 64;

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
    const std::size_t    uid_len = (user.size() > USER_ID_MAX) ? USER_ID_MAX : user.size();
    out.reserve(2 + 2 + uid_len + 2 + 4 + 2 + 32);
    out.push_back(MSG_CTRL_HELLO);
    out.push_back(HELLO_VER);
    // T_USER_ID only when length in 1..64
    if (uid_len > 0)
    {
        out.push_back(T_USER_ID);
        out.push_back(0x00);
        out.push_back(static_cast<uint8_t>(uid_len));
        out.insert(out.end(), user.begin(), user.begin() + uid_len);
    }
    out.push_back(T_CAPS);
    out.push_back(0x00);
    out.push_back(0x04);
    // explicit little-endian
    out.push_back(static_cast<uint8_t>((caps)&0xFF));
    out.push_back(static_cast<uint8_t>((caps >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((caps >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((caps >> 24) & 0xFF));

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
                // 1..64, else malformed
                if (L == 0 || L > USER_ID_MAX)
                    return false;
                h.user_id.assign(reinterpret_cast<const char *>(v), static_cast<size_t>(L));
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