#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace transport
{

using Frame   = std::vector<std::uint8_t>;
using OnFrame = std::function<void(const Frame &)>;

struct Settings
{
    std::string role;  // "central" or "peripheral" (or "loopback" for testing)
    std::string svc_uuid, tx_uuid, rx_uuid;
    std::size_t mtu_payload = 100;
};

struct ITransport
{
    virtual bool        start(const Settings &s, OnFrame on_rx) = 0;
    virtual bool        send(const Frame &one_chunk)            = 0;  // chunk == 1 BLE write
    virtual void        stop()                                  = 0;
    virtual std::string name() const { return ""; }
    virtual bool        link_ready() const = 0;
    virtual ~ITransport() = default;
};

}  // namespace transport
