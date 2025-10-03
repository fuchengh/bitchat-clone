/* ======================================================================
 * BlueZ Transport (facade) — overall flow
 *
 *  App thread                                   Transport (facade)                Central/Peripheral impl
 *  ----------                                   -------------------               -----------------------
 *  start(config, on_frame)
 *    └─ validate config / store callback
 *    └─ if role == central  ─────────────────────────────────────────────────────▶  start_central()
 *    └─ if role == peripheral ───────────────────────────────────────────────────▶  start_peripheral()
 *
 *  send_central(data, len)
 *    └─ checks role and link_ready
 *    └─ to central_write(data, len) ─────────────────────────────────────────────▶  DBus WriteValue (peer_rx)
 *
 *  send_peripheral(data, len)
 *    └─ checks role and link_ready
 *    └─ to send_peripheral_impl(data, len) ──────────────────────────────────────▶  emit PropertiesChanged
 *
 *  link_ready()
 *    └─ if central: returns connected && subscribed
 *    └─ if peripheral: returns tx_notifying() == true
 *
 *  stop()
 *    └─ if role == central  ─────────────────────────────────────────────────────▶  stop_central()
 *    └─ if role == peripheral ───────────────────────────────────────────────────▶  stop_peripheral()
 *    └─ clears state and threads
 *
 *  Threads
 *    └─ App thread calls facade APIs
 *    └─ Each role starts one bus loop thread to handle sd-bus I/O
 *
 *  Notes
 *    └─ All DBus calls issued from the app thread are under impl_->bus_mu
 *    └─ Callbacks from the bus thread deliver RX frames via on_frame
 * ====================================================================== */

#include <algorithm>
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
#include "transport/bluez_dbus_util.hpp"
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
const std::string &BluezTransport::adapter_path() const noexcept
{
    return impl_->adapter_path;
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

// ======================================================================
// Function: BluezTransport::start
// - In: config with role, UUIDs, adapter name
// - Out: returns true when role-specific start is kicked off
// - Note: validates config then delegates to central or peripheral
// ======================================================================
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
            LOG_INFO("[BLUEZ] mtu_payload overridden, env val = %zu", settings_.mtu_payload);
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

// ======================================================================
// Function: BluezTransport::stop
// - In: can be called anytime
// - Out: shuts down central or peripheral path as needed
// - Note: stop for the chosen role
// ======================================================================
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

// ======================================================================
// Function: BluezTransport::link_ready
// - In: none
// - Out: true when transport is usable for the current role
// - Note: central needs connected and subscribed, peripheral needs Notifying
// ======================================================================
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

// ======================================================================
// Function: BluezTransport::note_candidate
// - In: addr in AA:BB:CC:DD:EE:FF format, rssi (0 if unknown)
// - Out: updates or adds candidate to impl_->candidates
// ======================================================================
void BluezTransport::note_candidate(const std::string &addr, int16_t rssi)
{
#if !BITCHAT_HAVE_SDBUS
    (void)addr;
    (void)rssi;
    return;
#else
    if (!impl_)
        return;
    using namespace std::chrono;
    const uint64_t now_ms =
        (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    auto &m  = impl_->candidates;
    auto  it = m.find(addr);
    if (it == m.end())
    {
        impl_->candidates.emplace(addr, Impl::Candidate{addr, rssi, now_ms});
    }
    else
    {
        it->second.rssi         = rssi;
        it->second.last_seen_ms = now_ms;
    }
#endif
}

// ======================================================================
// Function: list_peers
// - In: none
// - Out: Make sure discovery=on, call ObjectManager.GetManagedObjects, return <MAC,RSSI> list
// - Note: Will not StopDiscovery
// ======================================================================
std::vector<PeerInfo> BluezTransport::list_peers()
{
#if !BITCHAT_HAVE_SDBUS
    return {};
#else
    std::vector<PeerInfo> out;
    if (!impl_ || !impl_->bus)
        return out;

    using namespace std::chrono;
    const uint64_t now_ms =
        (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    const uint64_t TTL_MS       = 120000;  // 120s
    bool           need_refresh = false;

    {
        std::lock_guard<std::mutex> lk(impl_->bus_mu);
        if (impl_->candidates.empty() ||
            now_ms - impl_->last_refresh_ms > impl_->refresh_min_interval_ms)
        {
            need_refresh = true;
        }
        for (const auto &kv : impl_->candidates)
        {
            const auto &c = kv.second;
            if (now_ms - c.last_seen_ms <= TTL_MS)
            {
                out.push_back(PeerInfo{c.addr, c.rssi});
            }
        }
    }
    if (need_refresh && impl_->bus)
    {
        impl_->refresh_req.store(true, std::memory_order_release);
    }

    std::sort(out.begin(), out.end(),
              [](const PeerInfo &a, const PeerInfo &b) { return a.rssi > b.rssi; });
    return out;

#endif
}

void BluezTransport::request_candidate_refresh()
{
#if !BITCHAT_HAVE_SDBUS
    return;
#else
    if (!impl_)
        return;
    impl_->refresh_req.store(true, std::memory_order_release);
#endif
}

}  // namespace transport
