#include <atomic>
#include <cstdint>
#include <errno.h>
#include <string>
#include <thread>
#include <utility>

#include "transport/bluez_transport.hpp"
#include "transport/itransport.hpp"
#include "util/log.hpp"

#if BITCHAT_HAVE_SDBUS
#include <systemd/sd-bus.h>
#include "bluez_helper.inc"

// GattService1 (svc_path)
static const sd_bus_vtable svc_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("UUID", "s", svc_prop_UUID, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Primary", "b", svc_prop_Primary, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Includes", "ao", svc_prop_Includes, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END};

// TX char (notify) + Start/StopNotify
static const sd_bus_vtable tx_vtable[] = {
    SD_BUS_VTABLE_START(0),
    // Properties (read-only unless noted)
    SD_BUS_PROPERTY("UUID", "s", chr_prop_UUID, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Service", "o", chr_prop_Service, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Flags", "as", chr_prop_Flags, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    // Notifying is dynamic (no CONST flag)
    SD_BUS_PROPERTY("Notifying", "b", chr_prop_Notifying, 0, 0),
    // Methods (unprivileged)
    SD_BUS_METHOD("StartNotify", "", "", tx_StartNotify, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("StopNotify", "", "", tx_StopNotify, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END};

// RX char (write)
static const sd_bus_vtable rx_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("UUID", "s", chr_prop_UUID, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Service", "o", chr_prop_Service, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Flags", "as", chr_prop_Flags, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_METHOD("WriteValue", "aya{sv}", "", rx_WriteValue, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END};

// LEAdvertisement1
static const sd_bus_vtable adv_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Type", "s", adv_prop_Type, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("ServiceUUIDs", "as", adv_prop_ServiceUUIDs, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("LocalName", "s", adv_prop_LocalName, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("IncludeTxPower",
                    "b",
                    adv_prop_IncludeTxPower,
                    0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_METHOD("Release", "", "", adv_Release, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END};

#endif

namespace transport
{
struct BluezTransport::Impl
{
#if BITCHAT_HAVE_SDBUS
    sd_bus *bus = nullptr;
    // peripheral
    sd_bus_slot     *app_slot      = nullptr;  // ObjectManager (/com/bitchat/app)
    sd_bus_slot     *svc_slot      = nullptr;  // GattService1 (/com/bitchat/app/svc0)
    sd_bus_slot     *tx_slot       = nullptr;  // TX characteristic
    sd_bus_slot     *rx_slot       = nullptr;  // RX characteristic
    sd_bus_slot     *adv_obj_slot  = nullptr;  // LEAdvertisement1 vtable
    sd_bus_slot     *adv_call_slot = nullptr;  // RegisterAdvertisement (async)
    std::string      adv_path      = "/com/bitchat/adv0";
    sd_bus_slot     *reg_slot      = nullptr;
    std::atomic_bool reg_ok{false};

    // central
    sd_bus_slot     *added_slot = nullptr;
    sd_bus_slot     *props_slot = nullptr;
    std::atomic_bool discovery_on{false};
    // central connection state flags
    std::atomic_bool connected{false};
    std::atomic_bool subscribed{false};
    std::atomic_bool connect_inflight{false};
    sd_bus_slot     *connect_call_slot{nullptr};
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
    std::string r_svc_path;  // remote service path
    std::string r_tx_path;   // remote TX characteristic (notify)
    std::string r_rx_path;   // remote RX characteristic (write)

    std::atomic_bool notifying{false};  // TX notify state
    std::string      unique_name;       // our bus unique name (debug)
};

BluezTransport::BluezTransport(BluezConfig cfg) : cfg_(std::move(cfg)) {}

BluezTransport::~BluezTransport()
{
    stop();
}

const std::string &BluezTransport::tx_path() const
{
    return impl_->tx_path;
}
const std::string &BluezTransport::rx_path() const
{
    return impl_->rx_path;
}
const std::string &BluezTransport::svc_path() const
{
    return impl_->svc_path;
}
const std::string &BluezTransport::app_path() const
{
    return impl_->app_path;
}
const std::string &BluezTransport::dev_path() const
{
    return impl_->dev_path;
}
const std::string &BluezTransport::unique_name() const
{
    return impl_->unique_name;
}

bool BluezTransport::tx_notifying() const
{
    return impl_->notifying.load();
}
void BluezTransport::set_tx_notifying(bool v)
{
    impl_->notifying.store(v);
}
void BluezTransport::set_reg_ok(bool v)
{
#if BITCHAT_HAVE_SDBUS
    impl_->reg_ok.store(v);
#endif
    // noop if sdbus is unavailable
    (void)v;
}
void BluezTransport::set_dev_path(const char *path)
{
    impl_->dev_path = std::string(path);
}

#if BITCHAT_HAVE_SDBUS
void BluezTransport::emit_tx_props_changed(const char *prop)
{
    if (!impl_ || !impl_->bus)
        return;
    sd_bus_emit_properties_changed(impl_->bus, impl_->tx_path.c_str(),
                                   "org.bluez.GattCharacteristic1", prop, nullptr);
}
#endif

std::string BluezTransport::name() const
{
    return "bluez";
}

bool BluezTransport::start(const Settings &s, OnFrame cb)
{
    if (running_.load(std::memory_order_relaxed))
        return true;

    settings_ = s;
    on_frame_ = std::move(cb);
    if (!impl_)
        impl_ = std::make_unique<Impl>();

    LOG_DEBUG("[BLUEZ] stub start: role=%s adapter=%s mtu_payload=%zu svc=%s tx=%s rx=%s%s%s",
              (cfg_.role == Role::Central ? "central" : "peripheral"), cfg_.adapter.c_str(),
              settings_.mtu_payload, cfg_.svc_uuid.c_str(), cfg_.tx_uuid.c_str(),
              cfg_.rx_uuid.c_str(), cfg_.peer_addr ? " peer=" : "",
              cfg_.peer_addr ? cfg_.peer_addr->c_str() : "");

    bool ok = (cfg_.role == Role::Peripheral) ? start_peripheral() : start_central();
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

bool BluezTransport::send(const Frame & /*frame*/)
{
    if (!running_.load(std::memory_order_relaxed))
        return false;
    // TODO: map Frame -> GATT Write
    LOG_DEBUG("[BLUEZ] stub send (frame)");
    return true;
}

void BluezTransport::stop()
{
    if (!running_.exchange(false, std::memory_order_relaxed))
        return;

    if (!impl_)
        return;

    if (cfg_.role == Role::Peripheral)
        stop_peripheral();
    else
        stop_central();

    LOG_DEBUG("[BLUEZ] stub stop");
    impl_.reset();
}

bool BluezTransport::start_peripheral()
{
#if !BITCHAT_HAVE_SDBUS
    LOG_ERROR("[BLUEZ][peripheral] sd-bus not available (BITCHAT_HAVE_SDBUS=0)");
    return false;
#else

    int r = (sd_bus_open_system(&impl_->bus));
    if (r < 0 || !impl_->bus)
    {
        LOG_ERROR("[BLUEZ][peripheral] failed to connect to system bus, err %d", r);
        return false;
    }
    impl_->adapter_path = "/org/bluez/" + cfg_.adapter;
    // not needed, but get a unique name for debugging
    const char *uniq_name = nullptr;
    if (sd_bus_get_unique_name(impl_->bus, &uniq_name) >= 0 && uniq_name)
        impl_->unique_name = uniq_name;

    r = sd_bus_add_object_manager(impl_->bus, &impl_->app_slot, impl_->app_path.c_str());
    if (r < 0)
    {
        LOG_ERROR("[BLUEZ][peripheral] add object manager failed: %d", r);
        return false;
    }

    // Service: org.bluez.GattService1 at /com/bitchat/app/svc0
    r = sd_bus_add_object_vtable(impl_->bus, &impl_->svc_slot, impl_->svc_path.c_str(),
                                 "org.bluez.GattService1", svc_vtable, /*userdata=*/this);
    if (r < 0)
    {
        LOG_ERROR("[BLUEZ][peripheral] add service vtable failed: %d", r);
        return false;
    }
    LOG_DEBUG("[BLUEZ][peripheral] service vtable exported at %s (bus=%s)",
              impl_->svc_path.c_str(), impl_->unique_name.c_str());
    // TX characteristic: org.bluez.GattCharacteristic1 (Flags=["notify"])
    r = sd_bus_add_object_vtable(impl_->bus, &impl_->tx_slot, impl_->tx_path.c_str(),
                                 "org.bluez.GattCharacteristic1", tx_vtable, /*userdata=*/this);
    if (r < 0)
    {
        LOG_ERROR("[BLUEZ][peripheral] add TX vtable failed: %d", r);
        return false;
    }
    // RX characteristic: org.bluez.GattCharacteristic1 (Flags=["write"])
    r = sd_bus_add_object_vtable(impl_->bus, &impl_->rx_slot, impl_->rx_path.c_str(),
                                 "org.bluez.GattCharacteristic1", rx_vtable, /*userdata=*/this);
    if (r < 0)
    {
        LOG_ERROR("[BLUEZ][peripheral] add RX vtable failed: %d", r);
        return false;
    }
    LOG_DEBUG("[BLUEZ][peripheral] char vtables exported: tx=%s rx=%s", impl_->tx_path.c_str(),
              impl_->rx_path.c_str());

    // Register application with BlueZ GattManager1 (async)
    r = sd_bus_call_method_async(impl_->bus, &impl_->reg_slot, "org.bluez",
                                 impl_->adapter_path.c_str(), "org.bluez.GattManager1",
                                 "RegisterApplication", on_reg_app_reply, this, "oa{sv}",
                                 impl_->app_path.c_str(), 0);
    if (r < 0)
    {
        LOG_ERROR("[BLUEZ][peripheral] RegisterApplication (async) submit failed: %s",
                  strerror(-r));
        return false;
    }

    r = sd_bus_add_object_vtable(impl_->bus, &impl_->adv_obj_slot, impl_->adv_path.c_str(),
                                 "org.bluez.LEAdvertisement1", adv_vtable, this);
    if (r < 0)
    {
        LOG_ERROR("[BLUEZ][peripheral] add adv vtable failed: %d", r);
        return false;
    }
    // Register with LEAdvertisingManager1 on the adapter (async)
    r = sd_bus_call_method_async(impl_->bus, &impl_->adv_call_slot, "org.bluez",
                                 impl_->adapter_path.c_str(), "org.bluez.LEAdvertisingManager1",
                                 "RegisterAdvertisement", on_reg_adv_reply, this, "oa{sv}",
                                 impl_->adv_path.c_str(),
                                 0  // empty options
    );
    if (r < 0)
    {
        LOG_ERROR("[BLUEZ][peripheral] RegisterAdvertisement submit failed: %s", strerror(-r));
        return false;
    }
    // event loop thread
    running_.store(true, std::memory_order_relaxed);
    impl_->loop = std::thread([this] {
        while (running_.load(std::memory_order_relaxed))
        {
            while (1)
            {
                int pr = sd_bus_process(impl_->bus, nullptr);
                if (pr <= 0)
                    break;
            }
            sd_bus_wait(impl_->bus, UINT64_MAX);
        }
    });

    return true;
#endif
}

void BluezTransport::stop_peripheral()
{
// clang-format off
#if BITCHAT_HAVE_SDBUS
    // unreg before closing the bus
    if (impl_->bus)
    {
        sd_bus_error    err{};
        sd_bus_message *rep = nullptr;
        (void)sd_bus_call_method(impl_->bus, "org.bluez", impl_->adapter_path.c_str(),
                                 "org.bluez.LEAdvertisingManager1", "UnregisterAdvertisement",
                                 &err, &rep, "o", impl_->adv_path.c_str());
        if (rep)
            sd_bus_message_unref(rep);
        sd_bus_error_free(&err);

        // Also unregister the GATT application
        err = {};
        rep = nullptr;
        (void)sd_bus_call_method(impl_->bus, "org.bluez", impl_->adapter_path.c_str(),
                                 "org.bluez.GattManager1", "UnregisterApplication", &err, &rep,
                                 "o", impl_->app_path.c_str());
        if (rep)
            sd_bus_message_unref(rep);
        sd_bus_error_free(&err);
    }

    // wake event loop if waiting in sd_bus_wait
    if (impl_->bus)
        sd_bus_close(impl_->bus);
 
    if (impl_->loop.joinable())
        impl_->loop.join();

    // cleanup
    if (impl_->reg_slot)      { sd_bus_slot_unref(impl_->reg_slot);      impl_->reg_slot = nullptr; }
    if (impl_->rx_slot)       { sd_bus_slot_unref(impl_->rx_slot);       impl_->rx_slot = nullptr; }
    if (impl_->tx_slot)       { sd_bus_slot_unref(impl_->tx_slot);       impl_->tx_slot = nullptr; }
    if (impl_->svc_slot)      { sd_bus_slot_unref(impl_->svc_slot);      impl_->svc_slot = nullptr; }
    if (impl_->app_slot)      { sd_bus_slot_unref(impl_->app_slot);      impl_->app_slot = nullptr; }
    if (impl_->adv_call_slot) { sd_bus_slot_unref(impl_->adv_call_slot); impl_->adv_call_slot = nullptr; }
    if (impl_->adv_obj_slot)  { sd_bus_slot_unref(impl_->adv_obj_slot);  impl_->adv_obj_slot = nullptr; }

    if (impl_->bus) {
        sd_bus_flush_close_unref(impl_->bus);
        impl_->bus = nullptr;
    }
// clang-format on
#endif
}

bool BluezTransport::start_central()
{
#if !BITCHAT_HAVE_SDBUS
    LOG_ERROR("[BLUEZ][central] sd-bus not available (BITCHAT_HAVE_SDBUS=0)");
    return false;
#else
    // connect system bus
    int r = sd_bus_open_system(&impl_->bus);
    if (r < 0 || !impl_->bus)
    {
        LOG_ERROR("[BLUEZ][central] failed to connect system bus: %d", r);
        return false;
    }

    // adapter path + unique name
    impl_->adapter_path = "/org/bluez/" + cfg_.adapter;
    const char *name    = nullptr;
    if (sd_bus_get_unique_name(impl_->bus, &name) >= 0 && name)
        impl_->unique_name = name;

    // subscribe signals
    r = sd_bus_match_signal(impl_->bus, &impl_->added_slot, "org.bluez", "/",
                            "org.freedesktop.DBus.ObjectManager", "InterfacesAdded",
                            on_iface_added, this);
    if (r < 0)
    {
        LOG_ERROR("[BLUEZ][central] subscribe to InterfacesAdded failed: %d", r);
        return false;
    }
    // PropertiesChanged (Device1.ServicesResolved / GattCharacteristic1.Value etc.)
    r = sd_bus_match_signal(impl_->bus, &impl_->props_slot, "org.bluez", "/",
                            "org.freedesktop.DBus.Properties", "PropertiesChanged",
                            on_props_changed, this);
    if (r < 0)
    {
        LOG_ERROR("[BLUEZ][central] subscribe to PropertiesChanged failed: %d", r);
        return false;
    }
    LOG_INFO("[BLUEZ][central] subscribed to InterfacesAdded/PropertiesChanged, svc=%s peer=%s",
             cfg_.svc_uuid.c_str(), cfg_.peer_addr ? cfg_.peer_addr->c_str() : "");

    (void)central_cold_scan();

    if (!central_start_discovery())
        LOG_WARN("[BLUEZ][central] StartDiscovery failed (continue without scan)");

    running_.store(true, std::memory_order_relaxed);
    impl_->loop = std::thread([this] {
        while (running_.load(std::memory_order_relaxed))
        {
            while (1)
            {
                int pr = sd_bus_process(impl_->bus, nullptr);
                if (pr <= 0)
                    break;
            }
            // state pump
            central_pump();
            sd_bus_wait(impl_->bus, UINT64_MAX);
        }
    });

    return true;
#endif
}

void BluezTransport::stop_central()
{
// clang-format off
#if BITCHAT_HAVE_SDBUS
    // StopDiscovery if on
    if (impl_->bus && impl_->discovery_on.load())
    {
        sd_bus_error    err{};
        sd_bus_message *rep = nullptr;
        (void)sd_bus_call_method(impl_->bus, "org.bluez", impl_->adapter_path.c_str(),
                                 "org.bluez.Adapter1", "StopDiscovery", &err, &rep, "");
        if (rep)
            sd_bus_message_unref(rep);
        sd_bus_error_free(&err);
        impl_->discovery_on.store(false);
    }

    // Wake the sd-bus event loop thread
    if (impl_->bus)
        sd_bus_close(impl_->bus);

    if (impl_->loop.joinable())
        impl_->loop.join();

    if (impl_->added_slot) { sd_bus_slot_unref(impl_->added_slot); impl_->added_slot = nullptr; }
    if (impl_->props_slot) { sd_bus_slot_unref(impl_->props_slot); impl_->props_slot = nullptr; }
    if (impl_->connect_call_slot) { sd_bus_slot_unref(impl_->connect_call_slot); impl_->connect_call_slot = nullptr; }

    impl_->connect_inflight.store(false);
    impl_->connected.store(false);
    impl_->subscribed.store(false);

    if (impl_->bus) {
        sd_bus_flush_close_unref(impl_->bus);
        impl_->bus = nullptr;
    }
// clang-format on
#endif
}

// -------- central utilities --------
bool BluezTransport::central_cold_scan()
{
#if !BITCHAT_HAVE_SDBUS
    return false;
#else
    // enumerate and try to seed dev_path() from cache
    sd_bus_message *reply = nullptr;
    sd_bus_error    err{};
    int r = sd_bus_call_method(impl_->bus, "org.bluez", "/", "org.freedesktop.DBus.ObjectManager",
                               "GetManagedObjects", &err, &reply, "");
    if (r < 0)
    {
        LOG_WARN("[BLUEZ][central] GetManagedObjects failed: %s",
                 err.message ? err.message : strerror(-r));
        sd_bus_error_free(&err);
        if (reply)
            sd_bus_message_unref(reply);
        return false;
    }

    const std::string dev_prefix = "/org/bluez/" + cfg_.adapter + "/dev_";
    const std::string want_uuid  = cfg_.svc_uuid;

    bool        found = false;
    std::string found_path;
    std::string addr;
    int16_t     rssi      = 0;
    bool        have_rssi = false;
    bool        svc_hit   = false;

    // Walk a{oa{sa{sv}}}
    r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "{oa{sa{sv}}}");
    if (r < 0)
        goto out;

    while ((r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_DICT_ENTRY, "oa{sa{sv}}")) > 0)
    {
        const char *obj = nullptr;
        if ((r = sd_bus_message_read(reply, "o", &obj)) < 0)
            goto out;
        if (!obj)
        {
            r = -EINVAL;
            goto out;
        }

        std::string path(obj);
        if (path.rfind(dev_prefix, 0) != 0)
        {
            // Skip non-device objects
            if ((r = sd_bus_message_skip(reply, "a{sa{sv}}")) < 0)
                goto out;
            if ((r = sd_bus_message_exit_container(reply)) < 0)
                goto out;
            continue;
        }

        // reset per-device state
        svc_hit = false;
        addr.clear();
        have_rssi = false;
        rssi      = 0;

        if ((r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "{sa{sv}}")) < 0)
            goto out;
        while ((r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_DICT_ENTRY, "sa{sv}")) > 0)
        {
            const char *iface = nullptr;
            if ((r = sd_bus_message_read(reply, "s", &iface)) < 0)
                goto out;
            if ((r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "{sv}")) < 0)
                goto out;

            if (iface && std::strcmp(iface, "org.bluez.Device1") == 0)
            {
                // Device1 props: UUIDs (as), Address (s), RSSI (n)
                while ((r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_DICT_ENTRY, "sv")) >
                       0)
                {
                    const char *key = nullptr;
                    if ((r = sd_bus_message_read(reply, "s", &key)) < 0)
                        goto out;
                    if (key && std::strcmp(key, "UUIDs") == 0)
                    {
                        bool hit = false;
                        if ((r = var_as_has_uuid(reply, want_uuid, hit)) < 0)
                            goto out;
                        svc_hit |= hit;
                    }
                    else if (key && std::strcmp(key, "Address") == 0)
                    {
                        if ((r = read_var_s(reply, addr)) < 0)
                            goto out;
                    }
                    else if (key && std::strcmp(key, "RSSI") == 0)
                    {
                        if ((r = read_var_i16(reply, rssi)) < 0)
                            goto out;
                        have_rssi = true;
                    }
                    else
                    {
                        if ((r = sd_bus_message_skip(reply, "v")) < 0)
                            goto out;
                    }
                    if ((r = sd_bus_message_exit_container(reply)) < 0)
                        goto out;  // dict-entry
                }
            }
            else
            {
                // skip other interfaces
                if ((r = sd_bus_message_skip(reply, "a{sv}")) < 0)
                    goto out;
            }

            if ((r = sd_bus_message_exit_container(reply)) < 0)
                goto out;  // a{sv}
            if ((r = sd_bus_message_exit_container(reply)) < 0)
                goto out;  // {sa{sv}}
        }
        if (r < 0)
            goto out;

        if ((r = sd_bus_message_exit_container(reply)) < 0)
            goto out;  // a{sa{sv}}
        if ((r = sd_bus_message_exit_container(reply)) < 0)
            goto out;  // {oa{sa{sv}}}

        // UUID hit + optional peer filter
        if (svc_hit)
        {
            if (!cfg_.peer_addr || cfg_.peer_addr->empty() || mac_eq(addr, *cfg_.peer_addr))
            {
                found      = true;
                found_path = std::move(path);
                break;  // first match is enough
            }
        }
    }
    if (r < 0)
        goto out;

    if (found && dev_path().empty())
    {
        set_dev_path(found_path.c_str());
        if (have_rssi)
        {
            LOG_DEBUG("[BLUEZ][central] cold-scan found %s addr=%s rssi=%d (svc hit)",
                      found_path.c_str(), addr.empty() ? "?" : addr.c_str(), (int)rssi);
        }
        else
        {
            LOG_DEBUG("[BLUEZ][central] cold-scan found %s addr=%s (svc hit)", found_path.c_str(),
                      addr.empty() ? "?" : addr.c_str());
        }
    }

out:
    sd_bus_message_unref(reply);
    sd_bus_error_free(&err);
    return r >= 0;
#endif
}

bool BluezTransport::central_start_discovery()
{
#if !BITCHAT_HAVE_SDBUS
    return false;
#else
    if (!impl_->bus)
        return false;
    sd_bus_error err{};
    sd_bus_message *rep = nullptr;
    int r = sd_bus_call_method(impl_->bus, "org.bluez", impl_->adapter_path.c_str(),
                               "org.bluez.Adapter1", "StartDiscovery", &err, &rep, "");
    if (rep)
        sd_bus_message_unref(rep);
    if (r < 0)
    {
        if (err.name && std::string(err.name) == "org.bluez.Error.InProgress")
        {
            impl_->discovery_on.store(true);
            LOG_INFO("[BLUEZ][central] StartDiscovery already in progress on %s",
                     impl_->adapter_path.c_str());
            sd_bus_error_free(&err);
            return true;
        }
        LOG_WARN("[BLUEZ][central] StartDiscovery failed: %s",
                 err.message ? err.message : strerror(-r));
        sd_bus_error_free(&err);
        return false;
    }
    sd_bus_error_free(&err);
    impl_->discovery_on.store(true);
    LOG_INFO("[BLUEZ][central] StartDiscovery OK on %s", impl_->adapter_path.c_str());
    return true;
#endif
}

bool BluezTransport::connected() const
{
    return impl_->connected.load();
}
void BluezTransport::set_connected(bool v)
{
    impl_->connected.store(v);
}
bool BluezTransport::subscribed() const
{
    return impl_->subscribed.load();
}
void BluezTransport::set_subscribed(bool v)
{
    impl_->subscribed.store(v);
}
void BluezTransport::set_connect_inflight(bool v)
{
    impl_->connect_inflight.store(v);
}

// --- in bluez_transport.cpp ---
bool BluezTransport::central_connect()
{
#if !BITCHAT_HAVE_SDBUS
    return false;
#else
    if (!impl_->bus || dev_path().empty())
        return false;

    if (impl_->connect_inflight.load() || connected())
        return true;

    int r = sd_bus_call_method_async(impl_->bus, &impl_->connect_call_slot, "org.bluez",
                                     dev_path().c_str(), "org.bluez.Device1", "Connect",
                                     on_connect_reply, this,
                                     ""  // no args
    );
    if (r < 0)
    {
        LOG_ERROR("[BLUEZ][central] submit Connect() failed: %s", strerror(-r));
        return false;
    }
    impl_->connect_inflight.store(true);
    LOG_DEBUG("[BLUEZ][central] Connect() submitted");
    return true;
#endif
}

bool BluezTransport::central_find_gatt_paths()
{
#if !BITCHAT_HAVE_SDBUS
    return false;
#else
    if (!impl_->bus || dev_path().empty())
        return false;

    sd_bus_message *reply = nullptr;
    sd_bus_error err{};
    int r = sd_bus_call_method(impl_->bus, "org.bluez", "/", "org.freedesktop.DBus.ObjectManager",
                               "GetManagedObjects", &err, &reply, "");
    if (r < 0)
    {
        LOG_WARN("[BLUEZ][central] GetManagedObjects failed: %s",
                 err.message ? err.message : strerror(-r));
        if (reply)
            sd_bus_message_unref(reply);
        sd_bus_error_free(&err);
        return false;
    }

    // Walk a{oa{sa{sv}}}
    r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "{oa{sa{sv}}}");
    if (r < 0)
    {
        sd_bus_message_unref(reply);
        sd_bus_error_free(&err);
        return false;
    }

    const std::string want_svc = cfg_.svc_uuid;
    const std::string want_tx = cfg_.tx_uuid;
    const std::string want_rx = cfg_.rx_uuid;
    const std::string dev_prefix = dev_path() + "/";

    while ((r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_DICT_ENTRY, "oa{sa{sv}}")) > 0)
    {
        const char *obj = nullptr;
        if ((r = sd_bus_message_read(reply, "o", &obj)) < 0)
            break;
        if (!obj)
        {
            r = -EINVAL;
            break;
        }

        std::string path(obj);
        // Only consider objects under our device path
        if (path.rfind(dev_prefix, 0) != 0)
        {
            // skip subtree
            if ((r = sd_bus_message_skip(reply, "a{sa{sv}}")) < 0)
                break;
            if ((r = sd_bus_message_exit_container(reply)) < 0)
                break;
            continue;
        }

        if ((r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "{sa{sv}}")) < 0)
            break;

        while ((r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_DICT_ENTRY, "sa{sv}")) > 0)
        {
            const char *iface = nullptr;
            if ((r = sd_bus_message_read(reply, "s", &iface)) < 0)
                break;

            if ((r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "{sv}")) < 0)
                break;

            if (iface && strcmp(iface, "org.bluez.GattService1") == 0)
            {
                // look for property "UUID" (variant(s))
                while ((r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_DICT_ENTRY, "sv")) >
                       0)
                {
                    const char *key = nullptr;
                    if ((r = sd_bus_message_read(reply, "s", &key)) < 0)
                        break;

                    if (key && strcmp(key, "UUID") == 0)
                    {
                        std::string uuid;
                        if ((r = read_var_s(reply, uuid)) < 0)
                            break;
                        if (ieq(uuid, want_svc))
                            impl_->r_svc_path = path;
                    }
                    else
                    {
                        if ((r = sd_bus_message_skip(reply, "v")) < 0)
                            break;
                    }
                    if ((r = sd_bus_message_exit_container(reply)) < 0)
                        break;
                }
            }
            else if (iface && strcmp(iface, "org.bluez.GattCharacteristic1") == 0)
            {
                // look for property "UUID" (variant(s))
                std::string uuid;
                while ((r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_DICT_ENTRY, "sv")) >
                       0)
                {
                    const char *key = nullptr;
                    if ((r = sd_bus_message_read(reply, "s", &key)) < 0)
                        break;

                    if (key && strcmp(key, "UUID") == 0)
                    {
                        if ((r = read_var_s(reply, uuid)) < 0)
                            break;
                    }
                    else
                    {
                        if ((r = sd_bus_message_skip(reply, "v")) < 0)
                            break;
                    }
                    if ((r = sd_bus_message_exit_container(reply)) < 0)
                        break;
                }
                // Match by UUID
                if (!uuid.empty())
                {
                    if (ieq(uuid, want_tx))
                        impl_->r_tx_path = path;
                    else if (ieq(uuid, want_rx))
                        impl_->r_rx_path = path;
                }
            }
            else
            {
                // skip
                if ((r = sd_bus_message_skip(reply, "a{sv}")) < 0)
                    break;
            }

            if ((r = sd_bus_message_exit_container(reply)) < 0)
                break;  // a{sv}
            if ((r = sd_bus_message_exit_container(reply)) < 0)
                break;  // {sa{sv}}
        }

        if (r < 0)
            break;
        if ((r = sd_bus_message_exit_container(reply)) < 0)
            break;  // a{sa{sv}}
        if ((r = sd_bus_message_exit_container(reply)) < 0)
            break;  // {oa{sa{sv}}}
    }

    sd_bus_message_unref(reply);
    sd_bus_error_free(&err);

    bool have_all = !impl_->r_svc_path.empty() && !impl_->r_tx_path.empty() &&
                    !impl_->r_rx_path.empty();
    if (have_all)
    {
        LOG_INFO("[BLUEZ][central] GATT discovered: svc=%s tx=%s rx=%s", impl_->r_svc_path.c_str(),
                 impl_->r_tx_path.c_str(), impl_->r_rx_path.c_str());
        return true;
    }
    return false;
#endif
}

bool BluezTransport::central_enable_notify()
{
#if !BITCHAT_HAVE_SDBUS
    return false;
#else
    if (!impl_->bus || impl_->r_tx_path.empty())
        return false;

    sd_bus_error err{};
    sd_bus_message *rep = nullptr;
    int r = sd_bus_call_method(impl_->bus, "org.bluez", impl_->r_tx_path.c_str(),
                               "org.bluez.GattCharacteristic1", "StartNotify", &err, &rep, "");
    if (rep)
        sd_bus_message_unref(rep);
    if (r < 0)
    {
        LOG_WARN("[BLUEZ][central] StartNotify failed: %s",
                 err.message ? err.message : strerror(-r));
        sd_bus_error_free(&err);
        return false;
    }
    sd_bus_error_free(&err);
    set_subscribed(true);
    LOG_INFO("[BLUEZ][central] Notifications enabled on %s", impl_->r_tx_path.c_str());
    return true;
#endif
}

bool BluezTransport::central_write(const uint8_t *data, size_t len)
{
#if !BITCHAT_HAVE_SDBUS
    (void)data;
    (void)len;
    return false;
#else
    if (!impl_->bus || impl_->r_rx_path.empty() || !data || len == 0)
        return false;

    sd_bus_message *msg = nullptr;
    sd_bus_message *rep = nullptr;
    sd_bus_error err{};
    int r = sd_bus_message_new_method_call(impl_->bus, &msg, "org.bluez", impl_->r_rx_path.c_str(),
                                           "org.bluez.GattCharacteristic1", "WriteValue");
    if (r < 0)
    {
        LOG_WARN("[BLUEZ][central] WriteValue new_method_call failed: %s", strerror(-r));
        return false;
    }

    // append ay payload
    r = sd_bus_message_append_array(msg, 'y', data, len);
    if (r < 0)
    {
        LOG_WARN("[BLUEZ][central] WriteValue append_array failed: %s", strerror(-r));
        sd_bus_message_unref(msg);
        return false;
    }

    // append empty options a{sv}
    r = sd_bus_message_open_container(msg, SD_BUS_TYPE_ARRAY, "{sv}");
    if (r < 0)
    {
        LOG_WARN("[BLUEZ][central] WriteValue open a{sv} failed: %s", strerror(-r));
        sd_bus_message_unref(msg);
        return false;
    }
    r = sd_bus_message_close_container(msg);
    if (r < 0)
    {
        LOG_WARN("[BLUEZ][central] WriteValue close a{sv} failed: %s", strerror(-r));
        sd_bus_message_unref(msg);
        return false;
    }
    r = sd_bus_call(impl_->bus, msg, 0, &err, &rep);
    sd_bus_message_unref(msg);
    if (rep)
        sd_bus_message_unref(rep);
    if (r < 0)
    {
        LOG_WARN("[BLUEZ][central] WriteValue failed: %s",
                 err.message ? err.message : strerror(-r));
        sd_bus_error_free(&err);
        return false;
    }
    sd_bus_error_free(&err);
    LOG_DEBUG("[BLUEZ][central] WriteValue OK (len=%zu)", len);
    return true;
#endif
}

// kick once when we have a device path
void BluezTransport::central_pump()
{
#if !BITCHAT_HAVE_SDBUS
    return;
#else
    if (!connected())
    {
        impl_->r_svc_path.clear();
        impl_->r_tx_path.clear();
        impl_->r_rx_path.clear();
    }

    // connect once when device is known
    if (!dev_path().empty() && !connected() && !impl_->connect_inflight.load())
    {
        (void)central_connect();
    }

    // discover GATT then enable notify
    if (connected() && !subscribed())
    {
        if (impl_->r_tx_path.empty() || impl_->r_rx_path.empty())
            (void)central_find_gatt_paths();

        if (!impl_->r_tx_path.empty() && !impl_->r_rx_path.empty())
            (void)central_enable_notify();
    }
    // when subscribed, stop discovery
    if (subscribed() && impl_->discovery_on.load() && impl_->bus)
    {
        sd_bus_error err{};
        sd_bus_message *rep = nullptr;
        int r = sd_bus_call_method(impl_->bus, "org.bluez", impl_->adapter_path.c_str(),
                                   "org.bluez.Adapter1", "StopDiscovery", &err, &rep, "");
        if (rep)
            sd_bus_message_unref(rep);
        if (r < 0)
        {
            LOG_WARN("[BLUEZ][central] StopDiscovery failed: %s",
                     err.message ? err.message : strerror(-r));
        }
        else
        {
            impl_->discovery_on.store(false);
            LOG_INFO("[BLUEZ][central] StopDiscovery OK");
        }
        sd_bus_error_free(&err);
    }
#endif
}

}  // namespace transport
