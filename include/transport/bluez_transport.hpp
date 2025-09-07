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

  private:
    BluezConfig      cfg_;
    Settings         settings_{};
    OnFrame          on_frame_{};
    std::atomic_bool running_{false};

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace transport