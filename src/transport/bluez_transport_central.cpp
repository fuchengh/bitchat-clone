// =============================================================================
// CENTRAL ROLE IMPLEMENTATION
// connect / discover / find GATT / enable notify / write / pump loop
//
// PUMP STATE MACHINE
//   1) If not connected: clear r_svc/r_tx/r_rx paths and discovery flags.
//   2) If dev_path empty: central_cold_scan() to seed target.
//   3) If have dev_path and !connected && !connect_inflight: central_connect()
//   4) If connected but not subscribed:
//        - wait ServicesResolved or central_discover_services()
//        - central_find_gatt_paths()
//        - central_enable_notify()
//   5) After subscribed: StopDiscovery if still running.
// =============================================================================

#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

// clang-format off
#include "transport/bluez_transport.hpp"
#include "transport/bluez_transport_impl.hpp"
#include "util/log.hpp"
#include "transport/bluez_dbus_util.hpp"
// clang-format on

#if BITCHAT_HAVE_SDBUS
#include <systemd/sd-bus.h>
#include "bluez_helper.inc"

namespace
{

// Tiny helper used only for early-exit.
static inline bool gatt_paths_ready(const std::string &svc,
                                    const std::string &tx,
                                    const std::string &rx)
{
    return !svc.empty() && !tx.empty() && !rx.empty();
}

static bool adapter_start_discovery_locked(sd_bus            *bus,
                                           const std::string &adapter_path,
                                           std::atomic_bool  &discovery_on)
{
    if (!bus)
        return false;
    if (discovery_on.load())
        return true;

    sd_bus_error    err{};
    sd_bus_message *rep = nullptr;
    int r = sd_bus_call_method(bus, "org.bluez", adapter_path.c_str(), "org.bluez.Adapter1",
                               "StartDiscovery", &err, &rep, "");
    if (rep)
        sd_bus_message_unref(rep);
    if (r < 0)
    {
        if (err.name && std::string(err.name) == "org.bluez.Error.InProgress")
        {
            discovery_on.store(true);
            LOG_INFO("[BLUEZ][central] StartDiscovery already in progress on %s",
                     adapter_path.c_str());
            sd_bus_error_free(&err);
            return true;
        }
        LOG_WARN("[BLUEZ][central] StartDiscovery failed: %s (ignoring as for now)",
                 err.message ? err.message : strerror(-r));
        sd_bus_error_free(&err);
        return false;
    }
    sd_bus_error_free(&err);
    discovery_on.store(true);
    LOG_SYSTEM("[BLUEZ][central] StartDiscovery OK on %s", adapter_path.c_str());
    return true;
}

static bool adapter_stop_discovery_locked(sd_bus            *bus,
                                          const std::string &adapter_path,
                                          std::atomic_bool  &discovery_on)
{
    if (!bus)
        return false;

    sd_bus_error    err{};
    sd_bus_message *rep = nullptr;
    int r = sd_bus_call_method(bus, "org.bluez", adapter_path.c_str(), "org.bluez.Adapter1",
                               "StopDiscovery", &err, &rep, "");
    if (rep)
        sd_bus_message_unref(rep);
    if (r < 0)
    {
        // These errors usually mean "already stopped" or state desync,
        // We still treat discovery_on as off to avoid stalling the caller's state machine.
        LOG_WARN("[BLUEZ][central] StopDiscovery failed (treat as off): %s",
                 err.message ? err.message : strerror(-r));
        discovery_on.store(false);
    }
    else
    {
        discovery_on.store(false);
        LOG_SYSTEM("[BLUEZ][central] StopDiscovery OK");
    }
    sd_bus_error_free(&err);
    return true;
}

static bool char_start_notify_locked(sd_bus            *bus,
                                     const std::string &peer_tx_path,
                                     std::atomic_bool  &subscribed)
{
    sd_bus_error    err{};
    sd_bus_message *rep = nullptr;
    int             r   = sd_bus_call_method(bus, "org.bluez", peer_tx_path.c_str(),
                                             "org.bluez.GattCharacteristic1", "StartNotify", &err, &rep, "");
    if (rep)
        sd_bus_message_unref(rep);
    if (r < 0)
    {
        const char *ename     = err.name ? err.name : "";
        const char *emsg      = err.message ? err.message : "";
        const bool  transient = std::strstr(emsg, "ATT error: 0x0e") != nullptr ||  // CCCD race
                               std::strcmp(ename, "org.freedesktop.DBus.Error.NoReply") ==
                                   0 ||  // BlueZ busy
                               std::strcmp(ename, "org.bluez.Error.InProgress") ==
                                   0;  // operation pending

        if (transient)
        {
            LOG_INFO("[BLUEZ][central] StartNotify transient failure (%s); will retry on next "
                     "pump",
                     emsg && *emsg ? emsg : ename);
        }
        else
        {
            LOG_WARN("[BLUEZ][central] StartNotify failed: %s",
                     emsg && *emsg ? emsg : strerror(-r));
        }
        sd_bus_error_free(&err);
        return false;  // allow central_pump() to retry
    }
    sd_bus_error_free(&err);
    subscribed.store(true);
    LOG_SYSTEM("[BLUEZ][central] Notifications enabled on %s", peer_tx_path.c_str());
    return true;
}

}  // namespace
#endif

namespace transport
{
// -------- Central state accessors --------
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

void BluezTransport::set_next_connect_at_ms(uint64_t new_ms)
{
    impl_->next_connect_at_ms = new_ms;
}

// -------- central utilities: discovery filter / control / connect / gatt / notify / write / pump --------
bool BluezTransport::central_set_discovery_filter()
{
#if !BITCHAT_HAVE_SDBUS
    return false;
#else
    if (!impl_->bus)
        return false;

    std::lock_guard<std::mutex> lk(impl_->bus_mu);

    sd_bus_message *msg = nullptr, *rep = nullptr;
    sd_bus_error    err{};
    int             r = sd_bus_message_new_method_call(impl_->bus, &msg, "org.bluez",
                                                       impl_->adapter_path.c_str(), "org.bluez.Adapter1",
                                                       "SetDiscoveryFilter");
    if (r < 0)
        goto out;

    // a{sv}
    r = sd_bus_message_open_container(msg, SD_BUS_TYPE_ARRAY, "{sv}");
    if (r < 0)
        goto out;
    // Transport="le"
    r = sd_bus_message_open_container(msg, SD_BUS_TYPE_DICT_ENTRY, "sv");
    if (r < 0)
        goto out;
    r = sd_bus_message_append(msg, "s", "Transport");
    if (r < 0)
        goto out;
    r = sd_bus_message_open_container(msg, SD_BUS_TYPE_VARIANT, "s");
    if (r < 0)
        goto out;
    r = sd_bus_message_append(msg, "s", "le");
    if (r < 0)
        goto out;
    r = sd_bus_message_close_container(msg);  // variant
    if (r < 0)
        goto out;
    r = sd_bus_message_close_container(msg);  // dict
    if (r < 0)
        goto out;
    // DuplicateData=false
    r = sd_bus_message_open_container(msg, SD_BUS_TYPE_DICT_ENTRY, "sv");
    if (r < 0)
        goto out;
    r = sd_bus_message_append(msg, "s", "DuplicateData");
    if (r < 0)
        goto out;
    r = sd_bus_message_open_container(msg, SD_BUS_TYPE_VARIANT, "b");
    if (r < 0)
        goto out;
    r = sd_bus_message_append(msg, "b", 0);
    if (r < 0)
        goto out;
    r = sd_bus_message_close_container(msg);  // variant
    if (r < 0)
        goto out;
    r = sd_bus_message_close_container(msg);  // dict
    if (r < 0)
        goto out;
    // UUIDs=["<svc_uuid>"]
    r = sd_bus_message_open_container(msg, SD_BUS_TYPE_DICT_ENTRY, "sv");
    if (r < 0)
        goto out;
    r = sd_bus_message_append(msg, "s", "UUIDs");
    if (r < 0)
        goto out;
    r = sd_bus_message_open_container(msg, SD_BUS_TYPE_VARIANT, "as");
    if (r < 0)
        goto out;
    r = sd_bus_message_open_container(msg, SD_BUS_TYPE_ARRAY, "s");
    if (r < 0)
        goto out;
    r = sd_bus_message_append(msg, "s", cfg_.svc_uuid.c_str());
    if (r < 0)
        goto out;
    r = sd_bus_message_close_container(msg);  // array
    if (r < 0)
        goto out;
    r = sd_bus_message_close_container(msg);  // variant
    if (r < 0)
        goto out;
    r = sd_bus_message_close_container(msg);  // dict
    if (r < 0)
        goto out;
    r = sd_bus_message_close_container(msg);  // a{sv}
    if (r < 0)
        goto out;

    r = sd_bus_call(impl_->bus, msg, 0, &err, &rep);
out:
    if (msg)
        sd_bus_message_unref(msg);
    if (rep)
        sd_bus_message_unref(rep);
    if (r < 0)
    {
        LOG_WARN("[BLUEZ][central] SetDiscoveryFilter failed: %s",
                 err.message ? err.message : strerror(-r));
        sd_bus_error_free(&err);
        set_uuid_discovery_filter_ok(false);
        return false;
    }
    sd_bus_error_free(&err);
    set_uuid_discovery_filter_ok(true);
    LOG_INFO("[BLUEZ][central] SetDiscoveryFilter OK (Transport=le, UUID=%s)",
             cfg_.svc_uuid.c_str());
    return true;
#endif
}

bool BluezTransport::central_connect()
{
#if !BITCHAT_HAVE_SDBUS
    return false;
#else
    if (!impl_->bus || dev_path().empty())
        return false;

    if (impl_->connect_inflight.load() || connected())
        return true;

    {
        std::lock_guard<std::mutex> lk(impl_->bus_mu);
        // Stop active discovery before connecting to avoid object churn/aborts.
        if (impl_->discovery_on.load())
        {
            bool stop_disc = adapter_stop_discovery_locked(impl_->bus, impl_->adapter_path,
                                                           impl_->discovery_on);
            if (!stop_disc)
            {
                LOG_SYSTEM("[BLUEZ][central] central connect failed when trying to stop "
                           "discovery");
            }
        }
        // Always submit Connect()
        int r = sd_bus_call_method_async(impl_->bus, &impl_->connect_call_slot, "org.bluez",
                                         dev_path().c_str(), "org.bluez.Device1", "Connect",
                                         on_connect_reply, this, "");
        if (r < 0)
        {
            LOG_ERROR("[BLUEZ][central] submit Connect() failed: %s", strerror(-r));
            return false;
        }
        impl_->connect_inflight.store(true);
    }

    LOG_DEBUG("[BLUEZ][central] Connect() submitted");
    return true;
#endif
}

bool BluezTransport::central_start_discovery()
{
#if !BITCHAT_HAVE_SDBUS
    return false;
#else
    if (!impl_ || !impl_->bus)
        return false;

    std::lock_guard<std::mutex> lk(impl_->bus_mu);
    return adapter_start_discovery_locked(impl_->bus, impl_->adapter_path, impl_->discovery_on);

#endif
}

bool BluezTransport::central_find_gatt_paths()
{
#if !BITCHAT_HAVE_SDBUS
    return false;
#else
    if (!impl_->bus || dev_path().empty())
        return false;

    std::lock_guard<std::mutex> lk(impl_->bus_mu);
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
    // ==========================
    // Hierarchy:
    // Object path (o)
    // |- Interfaces (a{sa{sv}})
    //     |- Properties ({sv})
    // ==========================
    // --- Objects
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

        // --- Interfaces
        if ((r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "{sa{sv}}")) < 0)
            break;
        while ((r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_DICT_ENTRY, "sa{sv}")) > 0)
        {
            const char *iface = nullptr;
            if ((r = sd_bus_message_read(reply, "s", &iface)) < 0)
                break;
            // Find properties in interface GattService1 or GattCharacteristic1
            if (iface && strcmp(iface, "org.bluez.GattService1") == 0)
            {
                // --- Service properties
                // (looking for property "UUID")
                if ((r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "{sv}")) < 0)
                    break;
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
                        {
                            impl_->peer_svc_path = path;
                            if (gatt_paths_ready(impl_->peer_svc_path, impl_->peer_tx_path,
                                                 impl_->peer_rx_path))
                                goto done_scan;
                        }
                    }
                    else
                    {
                        if ((r = sd_bus_message_skip(reply, "v")) < 0)
                            break;
                    }
                    if ((r = sd_bus_message_exit_container(reply)) < 0)
                        break;
                }
                if ((r = sd_bus_message_exit_container(reply)) < 0)
                    break;  // exit a{sv}
            }
            else if (iface && strcmp(iface, "org.bluez.GattCharacteristic1") == 0)
            {
                // --- Characteristic properties
                // (looking for property "UUID")
                std::string uuid;
                if ((r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "{sv}")) < 0)
                    break;
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
                    {
                        impl_->peer_tx_path = path;
                        if (gatt_paths_ready(impl_->peer_svc_path, impl_->peer_tx_path,
                                             impl_->peer_rx_path))
                            goto done_scan;
                    }
                    else if (ieq(uuid, want_rx))
                    {
                        impl_->peer_rx_path = path;
                        if (gatt_paths_ready(impl_->peer_svc_path, impl_->peer_tx_path,
                                             impl_->peer_rx_path))
                            goto done_scan;
                    }
                }
                if ((r = sd_bus_message_exit_container(reply)) < 0)
                    break;  // exit a{sv}
            }
            else
            {
                // skip
                if ((r = sd_bus_message_skip(reply, "a{sv}")) < 0)
                    break;
            }

            if ((r = sd_bus_message_exit_container(reply)) < 0)
                break;  // {sa{sv}} dict-entry
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

done_scan:
    bool have_all = !impl_->peer_svc_path.empty() && !impl_->peer_tx_path.empty() &&
                    !impl_->peer_rx_path.empty();
    if (have_all)
    {
        LOG_INFO("[BLUEZ][central] GATT discovered: svc=%s tx=%s rx=%s",
                 impl_->peer_svc_path.c_str(), impl_->peer_tx_path.c_str(),
                 impl_->peer_rx_path.c_str());
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
    if (!impl_ || !impl_->bus || impl_->peer_tx_path.empty())
        return false;

    std::lock_guard<std::mutex> lk(impl_->bus_mu);
    bool ok = char_start_notify_locked(impl_->bus, impl_->peer_tx_path, impl_->subscribed);
    if (ok && impl_->discovery_on.load())
    {
        (void)adapter_stop_discovery_locked(impl_->bus, impl_->adapter_path, impl_->discovery_on);
    }
    return ok;
#endif
}

bool BluezTransport::central_cold_scan()
{
#if !BITCHAT_HAVE_SDBUS
    return false;
#else
    std::lock_guard<std::mutex> lk(impl_->bus_mu);
    // enumerate and try to seed dev_path() from BlueZ cache
    sd_bus_message *reply = nullptr;
    sd_bus_error err{};
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
    const std::string want_uuid = cfg_.svc_uuid;

    bool found = false;
    std::string found_path;
    std::string addr;
    int16_t rssi = 0;
    bool have_rssi = false;
    bool svc_hit = false;

    // Walk a{oa{sa{sv}}}
    r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "{oa{sa{sv}}}");
    if (r < 0)
        goto out;
    // =========================
    // Hierarchy:
    // Object path (o)
    // |- Interfaces (a{sa{sv}})
    //     |- Properties ({sv})
    // =========================
    // --- Objects
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
            // Only process /org/bluez/<adapter>/dev_* objects, ignoring others
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
        rssi = 0;
        // --- Interfaces
        if ((r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "{sa{sv}}")) < 0)
            goto out;
        while ((r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_DICT_ENTRY, "sa{sv}")) > 0)
        {
            const char *iface = nullptr;
            if ((r = sd_bus_message_read(reply, "s", &iface)) < 0)
                goto out;

            // --- Properties: Looking for Device1 properties from Device1 interface
            if (iface && std::strcmp(iface, "org.bluez.Device1") == 0)
            {
                if ((r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "{sv}")) < 0)
                    goto out;
                // --- Found props: UUIDs (as), Address (s), RSSI (n)
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
                if ((r = sd_bus_message_exit_container(reply)) < 0)
                    goto out;  // exit a{sv}
            }
            else
            {
                // skip other interfaces
                if ((r = sd_bus_message_skip(reply, "a{sv}")) < 0)
                    goto out;
            }

            if ((r = sd_bus_message_exit_container(reply)) < 0)
                goto out;  // {sa{sv}} dict-entry
        }
        if (r < 0)
            goto out;

        if ((r = sd_bus_message_exit_container(reply)) < 0)
            goto out;  // a{sa{sv}}
        if ((r = sd_bus_message_exit_container(reply)) < 0)
            goto out;  // {oa{sa{sv}}}

        // Found candidate device, apply filtering logic:
        // - If has peer_addr set: match MAC or path, do not require svc_hit
        // - If no peer_addr set: require svc_hit
        if (cfg_.peer_addr && !cfg_.peer_addr->empty())
        {
            bool ok = (!addr.empty() && mac_eq(addr, *cfg_.peer_addr));
            if (!ok)
                ok = path_mac_eq(path, *cfg_.peer_addr);
            if (ok)
            {
                found = true;
                found_path = std::move(path);
                break;
            }
        }
        else if (svc_hit)
        {
            found = true;
            found_path = std::move(path);
            break;
        }
    }
    if (r < 0)
        goto out;

    // Only write dev_path() if we found a device and it was not already set.
    if (found && dev_path().empty())
    {
        set_dev_path(found_path.c_str());
        if (have_rssi)
        {
            LOG_SYSTEM("[BLUEZ][central] cold-scan found %s addr=%s rssi=%d (svc hit)",
                       found_path.c_str(), addr.empty() ? "?" : addr.c_str(), (int)rssi);
        }
        else
        {
            LOG_SYSTEM("[BLUEZ][central] cold-scan found %s addr=%s (svc hit)", found_path.c_str(),
                       addr.empty() ? "?" : addr.c_str());
        }
    }

out:
    sd_bus_message_unref(reply);
    sd_bus_error_free(&err);
    return r >= 0;
#endif
}

bool BluezTransport::central_write(const uint8_t *data, size_t len)
{
#if !BITCHAT_HAVE_SDBUS
    (void)data;
    (void)len;
    return false;
#else
    if (!impl_->bus || impl_->peer_rx_path.empty() || !data || len == 0)
        return false;

    std::lock_guard<std::mutex> lk(impl_->bus_mu);
    sd_bus_message *msg = nullptr;
    sd_bus_message *rep = nullptr;
    sd_bus_error err{};
    int r = sd_bus_message_new_method_call(impl_->bus, &msg, "org.bluez",
                                           impl_->peer_rx_path.c_str(),
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

    // options a{sv}: enforce "type=request" (Write Request, expect ATT response) and offset=0
    r = sd_bus_message_open_container(msg, SD_BUS_TYPE_ARRAY, "{sv}");
    if (r < 0)
    {
        LOG_WARN("[BLUEZ][central] WriteValue open a{sv} failed: %s", strerror(-r));
        sd_bus_message_unref(msg);
        return false;
    }
    // dict entry: "type" -> variant "s" = "request"
    r = sd_bus_message_open_container(msg, SD_BUS_TYPE_DICT_ENTRY, "sv");
    if (r < 0)
        goto build_opts_fail;
    r = sd_bus_message_append(msg, "s", "type");
    if (r < 0)
        goto build_opts_fail;
    r = sd_bus_message_open_container(msg, SD_BUS_TYPE_VARIANT, "s");
    if (r < 0)
        goto build_opts_fail;
    r = sd_bus_message_append(msg, "s", "request");
    if (r < 0)
        goto build_opts_fail;
    r = sd_bus_message_close_container(msg);  //variant
    if (r < 0)
        goto build_opts_fail;
    r = sd_bus_message_close_container(msg);  // dict-entry
    if (r < 0)
        goto build_opts_fail;
    // dict entry: "offset" -> variant "q" = 0
    r = sd_bus_message_open_container(msg, SD_BUS_TYPE_DICT_ENTRY, "sv");
    if (r < 0)
        goto build_opts_fail;
    r = sd_bus_message_append(msg, "s", "offset");
    if (r < 0)
        goto build_opts_fail;
    r = sd_bus_message_open_container(msg, SD_BUS_TYPE_VARIANT, "q");
    if (r < 0)
        goto build_opts_fail;
    r = sd_bus_message_append(msg, "q", (uint16_t)0);
    if (r < 0)
        goto build_opts_fail;
    r = sd_bus_message_close_container(msg);  //variant
    if (r < 0)
        goto build_opts_fail;
    r = sd_bus_message_close_container(msg);  //dict-entry
    if (r < 0)
        goto build_opts_fail;
    // close options map
    r = sd_bus_message_close_container(msg);  // a{sv}
    if (r < 0)
    {
        LOG_WARN("[BLUEZ][central] WriteValue close a{sv} failed: %s", strerror(-r));
        sd_bus_message_unref(msg);
        return false;
    }
    goto call_write;

build_opts_fail:
    LOG_WARN("[BLUEZ][central] WriteValue build options failed: %s", strerror(-r));
    sd_bus_message_unref(msg);
    return false;

call_write:
    r = sd_bus_call(impl_->bus, msg, 0, &err, &rep);
    sd_bus_message_unref(msg);
    if (rep)
        sd_bus_message_unref(rep);
    if (r < 0)
    {
        if (-r == EBADMSG)
        {
            // Some BlueZ builds can surface EBADMSG despite a successful ATT write.
            LOG_INFO("[BLUEZ][central] WriteValue returned EBADMSG; treating as soft error "
                     "(len=%zu)",
                     len);
        }
        else
        {
            LOG_WARN("[BLUEZ][central] WriteValue failed: %s",
                     err.message ? err.message : strerror(-r));
            sd_bus_error_free(&err);
            return false;
        }
    }

    sd_bus_error_free(&err);
    LOG_DEBUG("[BLUEZ][central] WriteValue OK (len=%zu)", len);
    return true;
#endif
}

void BluezTransport::central_pump()
{
// kick once when we have a device path
#if !BITCHAT_HAVE_SDBUS
    return;
#else
    if (!connected())
    {
        impl_->peer_svc_path.clear();
        impl_->peer_tx_path.clear();
        impl_->peer_rx_path.clear();
        impl_->discover_submitted.store(false);
    }

    // If we don't have a device yet, try to pick one from current BlueZ objects.
    if (dev_path().empty())
    {
        (void)central_cold_scan();
    }

    // connect once when device is known
    if (!dev_path().empty() && !connected() && !impl_->connect_inflight.load())
    {
        uint64_t now_ms =
            (uint64_t)std::chrono::duration_cast<
                std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count();
        if (now_ms >= impl_->next_connect_at_ms)
        {
            impl_->next_connect_at_ms = now_ms;
            (void)central_connect();
        }
    }

    if (connected() && !subscribed())
    {
        // Wait until services resolved to avoid spinning before objects exist
        if (!services_resolved())
            (void)central_discover_services(/*force_all=*/false);

        // try remote GATT even if no ServicesResolved is found
        if (central_find_gatt_paths())
        {
            if (central_enable_notify())
                LOG_SYSTEM("[BLUEZ][central] Notifications enabled; ready");
        }
        else
        {
            LOG_DEBUG("[BLUEZ][central] Connected, GATT not exported yet. Waiting...");
        }
    }

    // when subscribed, stop discovery
    if (subscribed() && impl_->discovery_on.load() && impl_->bus)
    {
        std::lock_guard<std::mutex> lk(impl_->bus_mu);
        bool stop_disc = adapter_stop_discovery_locked(impl_->bus, impl_->adapter_path,
                                                       impl_->discovery_on);
        if (!stop_disc)
        {
            LOG_SYSTEM("[BLUEZ][central] central pump failed when trying to stop discovery after "
                       "subscribed");
        }
    }
    else if (!subscribed() && impl_->bus)
    {
        std::lock_guard<std::mutex> lk(impl_->bus_mu);
        (void)adapter_start_discovery_locked(impl_->bus, impl_->adapter_path, impl_->discovery_on);
    }
#endif
}

bool BluezTransport::central_discover_services(bool force_all)
{
#if !BITCHAT_HAVE_SDBUS
    (void)force_all;
    return false;
#else
    if (!impl_->bus || dev_path().empty())
        return false;
    if (impl_->discover_submitted.load())
        return true;

    const char *pat = force_all ? "" : config().svc_uuid.c_str();
    std::lock_guard<std::mutex> lk(impl_->bus_mu);
    sd_bus_message *msg = nullptr, *rep = nullptr;
    sd_bus_error err{};
    int r = sd_bus_message_new_method_call(impl_->bus, &msg, "org.bluez", dev_path().c_str(),
                                           "org.bluez.Device1", "DiscoverServices");
    if (r < 0)
        goto out;

    r = sd_bus_message_append(msg, "s", pat);
    if (r < 0)
        goto out;

    r = sd_bus_call(impl_->bus, msg, 0, &err, &rep);
out:
    if (msg)
        sd_bus_message_unref(msg);
    if (rep)
        sd_bus_message_unref(rep);

    if (r < 0)
    {
        const char *ename = err.name ? err.name : "";
        if (strcmp(ename, "org.freedesktop.DBus.Error.UnknownMethod") == 0)
        {
            LOG_DEBUG("[BLUEZ][central] DiscoverServices not supported; rely on auto-discovery");
            impl_->discover_submitted.store(true);
            sd_bus_error_free(&err);
            return false;
        }
        LOG_WARN("[BLUEZ][central] DiscoverServices('%s') failed: %s",
                 force_all ? "" : config().svc_uuid.c_str(),
                 err.message ? err.message : strerror(-r));
        sd_bus_error_free(&err);
        return false;
    }
    sd_bus_error_free(&err);
    impl_->discover_submitted.store(true);
    LOG_INFO("[BLUEZ][central] DiscoverServices('%s') submitted",
             force_all ? "" : config().svc_uuid.c_str());
    return true;
#endif
}

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

// start/stop central
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
    const char *name = nullptr;
    if (sd_bus_get_unique_name(impl_->bus, &name) >= 0 && name)
        impl_->unique_name = name;

    // subscribe signals (iface added/removed)
    r = sd_bus_match_signal(impl_->bus, &impl_->added_slot, "org.bluez", "/",
                            "org.freedesktop.DBus.ObjectManager", "InterfacesAdded",
                            on_iface_added, this);
    if (r < 0)
    {
        LOG_ERROR("[BLUEZ][central] subscribe to InterfacesAdded failed: %d", r);
        return false;
    }
    r = sd_bus_match_signal(impl_->bus, &impl_->removed_slot, "org.bluez", "/",
                            "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved",
                            on_iface_removed, this);
    if (r < 0)
    {
        LOG_ERROR("[BLUEZ][central] subscribe to InterfacesRemoved failed: %d", r);
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

    (void)central_set_discovery_filter();

    if (!central_start_discovery())
        LOG_WARN("[BLUEZ][central] StartDiscovery failed (continue without scan)");

    running_.store(true, std::memory_order_relaxed);
    impl_->loop = std::thread([this] {
        while (running_.load(std::memory_order_relaxed))
        {
            {
                std::lock_guard<std::mutex> lk(impl_->bus_mu);
                while (1)
                {
                    int pr = sd_bus_process(impl_->bus, nullptr);
                    if (pr <= 0)
                        break;
                }
                // NOTE: callbacks invoked by sd_bus_process run in this same thread
                // and may call bus functions; that's fine while we hold the lock.
                const uint64_t WAIT_USEC = 100000;  // 100ms
                sd_bus_wait(impl_->bus, WAIT_USEC);
            }
            // do non-bus state work outside the lock
            central_pump();
        }
    });

    return true;
#endif
}

void BluezTransport::stop_central()
{
    // clang-format off
#if BITCHAT_HAVE_SDBUS
    // Do bus calls under the mutex, but NEVER join while holding the lock.
    {
        std::lock_guard<std::mutex> lk(impl_->bus_mu);
        // Disconnect active device (best-effort)
        if (impl_->bus && !impl_->dev_path.empty()) {
            sd_bus_error    derr{};
            sd_bus_message *drep = nullptr;
            (void)sd_bus_call_method(impl_->bus, "org.bluez", impl_->dev_path.c_str(),
                                     "org.bluez.Device1", "Disconnect", &derr, &drep, "");
            if (drep) sd_bus_message_unref(drep);
            sd_bus_error_free(&derr);
        }
        // StopDiscovery if on
        if (impl_->bus && impl_->discovery_on.load()) {
            bool stop_disc = adapter_stop_discovery_locked(impl_->bus, impl_->adapter_path,
                                                            impl_->discovery_on);
            if(!stop_disc)
            {
                LOG_SYSTEM("[BLUEZ][central] stop central failed when trying to stop discovery");
            }
        }
        // Wake the event loop thread if it's in sd_bus_wait()
        if (impl_->bus)
            sd_bus_close(impl_->bus);
    }

    // Join OUTSIDE of the mutex to avoid deadlocks with the loop thread.
    if (impl_->loop.joinable())
        impl_->loop.join();

    unref_slot(impl_->added_slot);
    unref_slot(impl_->removed_slot);
    unref_slot(impl_->props_slot);
    unref_slot(impl_->connect_call_slot);
    impl_->next_connect_at_ms = 0;

    impl_->connect_inflight.store(false);
    impl_->connected.store(false);
    impl_->subscribed.store(false);
    impl_->discover_submitted.store(false);

    if (impl_->bus) {
        sd_bus_flush_close_unref(impl_->bus);
        impl_->bus = nullptr;
    }
// clang-format on
#endif
}

}  // namespace transport