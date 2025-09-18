#include "transport/bluez_transport.hpp"
#include "util/log.hpp"

#include <chrono>
#include <thread>

namespace transport
{

bool BluezTransport::central_write_frame(const Frame &f)
{
    const size_t len = f.size();
    if (settings_.mtu_payload > 0 && len > settings_.mtu_payload)
    {
        LOG_WARN("[BLUEZ][central] send len=%zu > mtu_payload=%zu (sending anyway)", len,
                 settings_.mtu_payload);
    }
    const bool ok = central_write(f.data(), len);
    LOG_DEBUG("[BLUEZ][central] send len=%zu %s", len, ok ? "OK" : "FAIL");
    if (tx_pause_ms() > 0)
    {
        // Sleep after TX if configured (kept identical to previous behavior)
        std::this_thread::sleep_for(std::chrono::milliseconds(tx_pause_ms()));
    }
    return ok;
}

bool BluezTransport::send_central_impl(const Frame &f)
{
    return central_write_frame(f);
}

}  // namespace transport