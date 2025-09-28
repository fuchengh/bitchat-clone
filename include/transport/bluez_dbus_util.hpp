// include/transport/bluez_dbus_util.hpp
#pragma once
#include "transport/bluez_transport.hpp"

#include <cctype>
#include <cstdint>
#include <string>

#if BITCHAT_HAVE_SDBUS
#include <systemd/sd-bus.h>
#endif

static inline bool ieq(std::string a, std::string b)
{
    auto norm = [](std::string s) {
        for (auto &c : s)
            c = (char)std::tolower((unsigned char)c);
        return s;
    };
    return norm(std::move(a)) == norm(std::move(b));
}

static inline bool mac_eq(std::string a, std::string b)
{
    auto norm = [](std::string s) {
        for (auto &c : s)
            c = (char)std::toupper((unsigned char)c);
        return s;
    };
    return norm(std::move(a)) == norm(std::move(b));
}

[[maybe_unused]] static inline bool path_mac_eq(const std::string &obj_path,
                                                const std::string &mac)
{
    // DBus path "/org/bluez/hci0/dev_XX_YY_ZZ" vs "XX:YY:ZZ"
    auto pos = obj_path.rfind("/dev_");
    if (pos == std::string::npos)
        return false;
    std::string tail = obj_path.substr(pos + 5);
    for (auto &c : tail)
        if (c == '_')
            c = ':';
    return mac_eq(tail, mac);
}

#if BITCHAT_HAVE_SDBUS
[[maybe_unused]] static inline int read_var_s(sd_bus_message *m, std::string &out)
{
    // read variant "s"
    int r = sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, "s");
    if (r < 0)
        return r;
    const char *s = nullptr;
    r             = sd_bus_message_read(m, "s", &s);
    if (r >= 0 && s)
        out = s;
    int r2 = sd_bus_message_exit_container(m);
    return r < 0 ? r : r2;
}

[[maybe_unused]] static inline int read_var_i16(sd_bus_message *m, int16_t &out)
{
    // read variant "n" (int16)
    int r = sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, "n");
    if (r < 0)
        return r;
    r      = sd_bus_message_read(m, "n", &out);
    int r2 = sd_bus_message_exit_container(m);
    return r < 0 ? r : r2;
}

[[maybe_unused]] static inline int var_as_has_uuid(sd_bus_message    *m,
                                                   const std::string &want_uuid,
                                                   bool              &hit)
{
    // read variant "as" and check if list contains want_uuid (case-insensitive)
    hit   = false;
    int r = sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, "as");
    if (r < 0)
        return r;
    r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "s");
    if (r < 0)
        return r;
    while (true)
    {
        const char *u  = nullptr;
        int         rr = sd_bus_message_read_basic(m, 's', &u);
        if (rr <= 0)
            break;
        if (u && ieq(u, want_uuid))
            hit = true;
    }
    int r1 = sd_bus_message_exit_container(m);
    int r2 = sd_bus_message_exit_container(m);
    return (r < 0 || r1 < 0 || r2 < 0) ? -1 : 0;
}
#endif