#pragma once
#include <cstddef>
#include <string_view>

namespace constants
{
// UUIDs (RFC4122 v4) — fixed for bitchat-clone MVP-Lite
inline constexpr std::string_view SVC_UUID = "7e0f8f20-cc0b-4c6e-8a3e-5d21b2f8a9c4";
inline constexpr std::string_view TX_UUID  = "7e0f8f21-cc0b-4c6e-8a3e-5d21b2f8a9c4";  // Notify
inline constexpr std::string_view RX_UUID =
    "7e0f8f22-cc0b-4c6e-8a3e-5d21b2f8a9c4";  // Write w/ response

// Control socket path (Unix domain socket)
inline constexpr std::string_view CTL_SOCK_PATH = "~/.cache/bitchat-clone/ctl.sock";
}  // namespace constants
