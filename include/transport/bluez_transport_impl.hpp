// include/transport/bluez_transport_impl.hpp
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
struct sd_bus;
struct sd_bus_slot;

#include "bluez_transport.hpp"

namespace transport
{
struct BluezTransport::Impl
{
#if BITCHAT_HAVE_SDBUS
    sd_bus *bus = nullptr;
    // peripheral
    sd_bus_slot *app_slot      = nullptr;  // ObjectManager (/com/bitchat/app)
    sd_bus_slot *svc_slot      = nullptr;  // GattService1 (/com/bitchat/app/svc0)
    sd_bus_slot *tx_slot       = nullptr;  // TX characteristic
    sd_bus_slot *rx_slot       = nullptr;  // RX characteristic
    sd_bus_slot *adv_obj_slot  = nullptr;  // LEAdvertisement1 vtable
    sd_bus_slot *adv_call_slot = nullptr;  // RegisterAdvertisement (async)
    std::string  adv_path      = "/com/bitchat/adv0";
    sd_bus_slot *reg_slot      = nullptr;

    // serialize all sd-bus access
    std::mutex bus_mu;

    // central
    sd_bus_slot *added_slot   = nullptr;
    sd_bus_slot *removed_slot = nullptr;

    sd_bus_slot     *props_slot = nullptr;
    std::atomic_bool discovery_on{false};
    sd_bus_slot     *connect_call_slot{nullptr};
    bool             uuid_filter_ok{false};
#endif
    std::thread loop;
    // peripheral state
    std::string adapter_path;  // "/org/bluez/hci0"
    std::string app_path = "/com/bitchat/app";
    std::string svc_path = "/com/bitchat/app/svc0";
    std::string tx_path  = "/com/bitchat/app/svc0/char_tx";
    std::string rx_path  = "/com/bitchat/app/svc0/char_rx";
    // central state
    std::string dev_path;
    std::string peer_svc_path;  // remote service path
    std::string peer_tx_path;   // remote TX characteristic (notify)
    std::string peer_rx_path;   // remote RX characteristic (write)
    // central connection state flags
    std::atomic_bool connected{false};
    std::atomic_bool subscribed{false};
    std::atomic_bool connect_inflight{false};
    std::atomic_bool services_resolved{false};
    std::atomic_bool discover_submitted{false};
    uint64_t         next_connect_at_ms{0};
    // inter-fragment pause in ms
    uint32_t tx_pause_ms{100};

    std::atomic_bool notifying{false};  // TX notify state
    std::string      unique_name;       // our bus unique name (debug)
};
}  // namespace transport