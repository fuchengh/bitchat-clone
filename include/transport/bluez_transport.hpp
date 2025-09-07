#pragma once
#include <atomic>
#include <memory>
#include <optional>
#include <string>

#include "transport/itransport.hpp"
#include "util/constants.hpp"

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

    const std::string &tx_path() const;
    const std::string &rx_path() const;
    const std::string &svc_path() const;
    const std::string &app_path() const;
    const std::string &unique_name() const;
    bool               tx_notifying() const;
    void               set_tx_notifying(bool v);
    void               set_reg_ok(bool v);
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

    // peripheral-side helpers
    bool start_peripheral();
    void stop_peripheral();
    // central-side helpers (TODO)
    bool start_central()
    {
        return true;
    }
    void stop_central() {}
};

}  // namespace transport