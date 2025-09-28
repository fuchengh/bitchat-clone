#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "transport/itransport.hpp"
#include "util/constants.hpp"

#if BITCHAT_HAVE_SDBUS
#include <poll.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

namespace
{
// TU-local wrapper to unref and null a slot ptr
inline void unref_slot(sd_bus_slot *&s)
{
    if (s)
    {
        sd_bus_slot_unref(s);
        s = nullptr;
    }
}
}  // namespace

#endif

namespace transport
{

enum class Role
{
    Central,
    Peripheral
};

struct BluezConfig
{
    Role                       role     = Role::Peripheral;
    std::string                adapter  = "hci0";
    std::string                svc_uuid = std::string(constants::SVC_UUID);
    std::string                tx_uuid  = std::string(constants::TX_UUID);  // Notify
    std::string                rx_uuid = std::string(constants::RX_UUID);  // Write (with response)
    std::optional<std::string> peer_addr{};
};

class BluezTransport final : public ITransport
{
  public:
    explicit BluezTransport(BluezConfig cfg);
    ~BluezTransport() override;

    bool        start(const Settings &s, OnFrame cb) override;
    bool        send(const Frame &f) override;
    void        stop() override;
    std::string name() const override;

    const BluezConfig &config() const { return cfg_; }

    // set/get accessors to impl_ members
    const std::string &tx_path() const;
    const std::string &rx_path() const;
    const std::string &svc_path() const;
    const std::string &app_path() const;
    const std::string &dev_path() const;
    const std::string &unique_name() const;
    bool               tx_notifying() const;
    void               set_tx_notifying(bool v);
    void               set_dev_path(const char *path);
    bool               connected() const;
    void               set_connected(bool v);
    bool               subscribed() const;
    void               set_subscribed(bool v);
    void               set_connect_inflight(bool v);
    bool     is_running() const noexcept { return running_.load(std::memory_order_relaxed); }
    void     deliver_rx_bytes(const uint8_t *data, size_t len);
    void     set_next_connect_at_ms(uint64_t new_ms);
    bool     services_resolved() const;
    void     set_services_resolved(bool v);
    bool     has_uuid_discovery_filter() const;
    void     set_uuid_discovery_filter_ok(bool v);
    uint32_t tx_pause_ms() const;
#if BITCHAT_HAVE_SDBUS
    std::mutex &bus_mu();
    sd_bus     *bus();
#endif

    bool central_enable_notify();
    bool central_find_gatt_paths();
    bool central_discover_services(bool force_all = false);

    bool link_ready() const override;

#if BITCHAT_HAVE_SDBUS
    void emit_tx_props_changed(const char *prop);
#endif

  private:
    BluezConfig      cfg_;
    Settings         settings_{};
    OnFrame          on_frame_{};
    std::atomic_bool running_{false};

    struct Impl;
    std::unique_ptr<Impl> impl_;

    // start/stop handlers
    bool (BluezTransport::*start_role_)() = nullptr;  // start_central | start_peripheral
    void (BluezTransport::*stop_role_)()  = nullptr;  // stop_central | stop_peripheral

    bool (BluezTransport::*send_role_)(const Frame &) = nullptr;  // peri_impl | central_impl

    // peripheral-side helpers
    bool start_peripheral();
    void stop_peripheral();
    bool send_peripheral_impl(const Frame &frame);
    bool peripheral_notify_value(const Frame &f);
    bool peripheral_can_notify();
    bool peripheral_send_locked(const Frame &f);

    // central-side helpers
    bool start_central();
    void stop_central();
    bool send_central_impl(const Frame &frame);
    bool central_write_frame(const Frame &f);
    bool central_cold_scan();
    bool central_start_discovery();
    bool central_connect();
    bool central_write(const uint8_t *data, size_t len);
    bool central_set_discovery_filter();
    void central_pump();
};

}  // namespace transport