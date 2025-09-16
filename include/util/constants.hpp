#pragma once
#include <cstddef>
#include <string>
#include <string_view>

#include "util/log.hpp"

namespace constants
{
// UUIDs (RFC4122 v4) — fixed for bitchat-clone MVP-Lite
inline constexpr std::string_view SVC_UUID = "7e0f8f20-cc0b-4c6e-8a3e-5d21b2f8a9c4";
inline constexpr std::string_view TX_UUID  = "7e0f8f21-cc0b-4c6e-8a3e-5d21b2f8a9c4";  // Notify
inline constexpr std::string_view RX_UUID =
    "7e0f8f22-cc0b-4c6e-8a3e-5d21b2f8a9c4";  // Write w/ response

// Control socket path (Unix domain socket)
[[maybe_unused]] static std::string ctl_sock_path()
{
    if (const char *p = std::getenv("BITCHAT_CTL_SOCK"); p && *p)
    {
        return std::string(p);
    }
    // fallback to default
    const char *home = std::getenv("HOME");
    std::string base = home && *home ? std::string(home) : "/tmp";
    std::string sock_path = base + "/.cache/bitchat-clone/ctl.sock";
    LOG_SYSTEM("Listening on %s", sock_path.c_str());
    return sock_path;
}

}  // namespace constants
