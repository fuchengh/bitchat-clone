// ===========================================================================================
// BluezTransport (facade / role-agnostic entry points)
//
// ROLE-LEVEL READY SIGNAL:
//  - Central: ready == (connected && subscribed)
//  - Peripheral: ready == (Notifying == true) on TX characteristic
//
// LIFECYCLE CHECKLIST:
//  - Peripheral: Stop advertising -> unexport objects/vtables -> clear slots -> stop loop
//  - Central: Disable notifications -> cancel inflight calls -> StopDiscovery if needed ->
//             unhook Added/Removed/Props slots -> stop loop
// ===========================================================================================

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

// clang-format off
#include "transport/bluez_transport.hpp"
#include "transport/bluez_transport_impl.hpp"
#include "transport/itransport.hpp"
#include "util/log.hpp"
// clang-format on

#if BITCHAT_HAVE_SDBUS
#include <systemd/sd-bus.h>
#endif
namespace transport
{
BluezTransport::BluezTransport(BluezConfig cfg) : cfg_(std::move(cfg))
{
    // select role handlers
    if (cfg_.role == Role::Peripheral)
    {
        start_role_ = &BluezTransport::start_peripheral;
        stop_role_  = &BluezTransport::stop_peripheral;
        send_role_  = &BluezTransport::send_peripheral_impl;
    }
    else
    {
        start_role_ = &BluezTransport::start_central;
        stop_role_  = &BluezTransport::stop_central;
        send_role_  = &BluezTransport::send_central_impl;
    }
}

BluezTransport::~BluezTransport()
{
    stop();
}

// ============== Impl accessors ==============
const std::string &BluezTransport::tx_path() const noexcept
{
    return impl_->tx_path;
}
const std::string &BluezTransport::rx_path() const noexcept
{
    return impl_->rx_path;
}
const std::string &BluezTransport::svc_path() const noexcept
{
    return impl_->svc_path;
}
const std::string &BluezTransport::app_path() const noexcept
{
    return impl_->app_path;
}
const std::string &BluezTransport::dev_path() const noexcept
{
    return impl_->dev_path;
}
const std::string &BluezTransport::unique_name() const noexcept
{
    return impl_->unique_name;
}
bool BluezTransport::tx_notifying() const noexcept
{
    return impl_->notifying.load();
}
void BluezTransport::set_tx_notifying(bool v)
{
    impl_->notifying.store(v);
}
void BluezTransport::set_dev_path(const char *path)
{
    impl_->dev_path = std::string(path);
}
bool BluezTransport::services_resolved() const
{
    return impl_ ? impl_->services_resolved.load() : false;
}
void BluezTransport::set_services_resolved(bool v)
{
    if (impl_)
        impl_->services_resolved.store(v);
}
bool BluezTransport::has_uuid_discovery_filter() const
{
#if BITCHAT_HAVE_SDBUS
    return impl_ && impl_->uuid_filter_ok;
#else
    return false;
#endif
}
void BluezTransport::set_uuid_discovery_filter_ok(bool v)
{
#if BITCHAT_HAVE_SDBUS
    if (impl_)
        impl_->uuid_filter_ok = v;
#else
    (void)v;
#endif
}
uint32_t BluezTransport::tx_pause_ms() const
{
    return impl_ ? impl_->tx_pause_ms : 0;
}
#if BITCHAT_HAVE_SDBUS
std::mutex &BluezTransport::bus_mu()
{
    if (!impl_)
    {
        static std::mutex dummy_mu;
        return dummy_mu;
    }
    return impl_->bus_mu;
}
sd_bus *BluezTransport::bus()
{
    return impl_ ? impl_->bus : nullptr;
}
#endif
// ============= End of Impl accessors =============

std::string BluezTransport::name() const
{
    return "bluez";
}

void BluezTransport::deliver_rx_bytes(const uint8_t *data, size_t len)
{
    // contract: transport layer frames -> upper layer defrag/decode
    // MUST be thread-safe at the caller side

    if (!data || len == 0)
        return;
    if (!is_running())
        return;
    if (!on_frame_)
        return;

    Frame f{};
    f.resize(len);
    std::memcpy(f.data(), data, len);
    on_frame_(std::move(f));
}

bool BluezTransport::start(const Settings &s, OnFrame cb)
{
    if (running_.load(std::memory_order_relaxed))
        return true;

    settings_ = s;
    on_frame_ = std::move(cb);
    if (!impl_)
        impl_ = std::make_unique<Impl>();

    // Env override for mtu_payload to allow faster communication
    if (const char *e = std::getenv("BITCHAT_MTU_PAYLOAD"))
    {
        char         *p = nullptr;
        unsigned long v = std::strtoul(e, &p, 10);
        if (p && *p == '\0' && v >= 20 && v <= 244)
        {
            settings_.mtu_payload = static_cast<size_t>(v);
            LOG_INFO("[BLUEZ] mtu_payload overrided, env val = %zu", settings_.mtu_payload);
        }
    }

    LOG_DEBUG("[BLUEZ][%s] stub start: adapter=%s mtu_payload=%zu svc=%s tx=%s rx=%s%s%s",
              (cfg_.role == Role::Central ? "central" : "peripheral"), cfg_.adapter.c_str(),
              settings_.mtu_payload, cfg_.svc_uuid.c_str(), cfg_.tx_uuid.c_str(),
              cfg_.rx_uuid.c_str(), cfg_.peer_addr ? " peer=" : "",
              cfg_.peer_addr ? cfg_.peer_addr->c_str() : "");

    bool ok = (this->*start_role_)();
    if (ok)
    {
        running_.store(true, std::memory_order_relaxed);
    }
    else
    {
        impl_.reset();
    }
    return ok;
}

bool BluezTransport::send(const Frame &f)
{
    if (!running_.load(std::memory_order_relaxed))
        return false;
    if (f.empty())
        return false;
    // Role-dispatch
    return (this->*send_role_)(f);
}

void BluezTransport::stop()
{
    // STOP SEQUENCE (facade):
    // - Switch by role to call stop_central() / stop_peripheral().
    // - Ensure bus thread is joined OUTSIDE of any locks.
    // - Clear Impl last so any callbacks see a consistent "stopped" state.

    if (!running_.exchange(false, std::memory_order_relaxed))
        return;

    if (!impl_)
        return;

    (this->*stop_role_)();

    LOG_DEBUG("[BLUEZ] stub stop");
    impl_.reset();
}

bool BluezTransport::link_ready() const
{
    if (!impl_)
        return false;
    if (cfg_.role == Role::Central)
        return impl_->connected.load() && impl_->subscribed.load();
    // Peripheral: ready when notifications are on (identical to pre-refactor behavior)
    // WARNING: the 'notifying' flag is toggled only from the bus thread via Start/StopNotify.
    return impl_->notifying.load();
}

}  // namespace transport
