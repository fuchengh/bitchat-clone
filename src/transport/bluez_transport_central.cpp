/* ======================================================================
 * BlueZ Central — normal path
 *
 *  App thread                       Bus thread                 BlueZ/DBus           Peer (Peripheral)
 *  ----------                       -----------                -----------          ------------------
 *  start_central()
 *    └─ set_discovery_filter ─────────────────────────────────────────────────────▶  Adapter.SetDiscoveryFilter
 *    └─ start_discovery ──────────────────────────────────────────────────────────▶  Adapter.StartDiscovery
 *    └─ spawn bus loop
 *
 *  central_pump()
 *    └─ cold_scan (cache) ────────────────────────────────────────────────────────▶  ObjectManager.GetManagedObjects
 *    └─ if have dev_path → connect ───────────────────────────────────────────────▶  Device1.Connect
 *                                         ◀── on_connect_reply (success) ──────────
 *    └─ discover_services ────────────────────────────────────────────────────────▶  Device1.DiscoverServices
 *                                         ◀── on_props_changed: ServicesResolved=true
 *    └─ find_gatt_paths (svc/tx/rx from cache)
 *    └─ enable_notify ────────────────────────────────────────────────────────────▶  GattCharacteristic1.StartNotify
 *                                         ◀── on_props_changed: Notifying=true
 *    └─ stop_discovery ───────────────────────────────────────────────────────────▶  Adapter.StopDiscovery
 *
 *  Data
 *    send_central(data) ──────────────────────────────────────────────────────────▶  GattCharacteristic1.WriteValue (peer_rx)
 *                                         ◀────────────────── notifications (peer_tx → Notifying)
 *
 *  Ready condition: connected && subscribed
 *  DBus calls on app thread are under impl_->bus_mu
 * ====================================================================== */

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
#include "transport/bluez_helper_central.hpp"
namespace
{

// ======================================================================
// Function: gatt_paths_ready
// - In: three object paths
// - Out: true when all are non empty
// - Note: small readability helper
// ======================================================================
static inline bool gatt_paths_ready(const std::string &svc,
                                    const std::string &tx,
                                    const std::string &rx)
{
    return !svc.empty() && !tx.empty() && !rx.empty();
}

// ======================================================================
// Function: adapter_start_discovery_locked
// - In: bus_mu locked, adapter_path valid
// - Out: true if StartDiscovery succeeds and discovery_on becomes true
// - Note: safe to call repeatedly, only starts when off
// ======================================================================
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

// ======================================================================
// Function: adapter_stop_discovery_locked
// - In: bus_mu locked, adapter_path valid
// - Out: true if StopDiscovery succeeds or already off
// - Note: clears discovery_on even if StopDiscovery fails
// ======================================================================
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

// ======================================================================
// Function: char_start_notify_locked
// - In: bus must be valid, bus_mu locked, peer_tx_path is the TX characteristic
// - Out: true if StartNotify returns success on DBus
// - Note: sets subscribed to true when successful
// ======================================================================
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
bool BluezTransport::connected() const noexcept
{
    return impl_->connected.load();
}

void BluezTransport::set_connected(bool v)
{
    impl_->connected.store(v);
}

bool BluezTransport::subscribed() const noexcept
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
// ======================================================================
// Function: BluezTransport::central_set_discovery_filter
// - In: bus valid, sets Adapter1.SetDiscoveryFilter to LE and our service UUID
// - Out: true on success and marks uuid_filter_ok
// - Note: reduces scan noise and speeds up find
// ======================================================================
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
        if (-r == EBADMSG)
        {
            LOG_INFO("[BLUEZ][central] SetDiscoveryFilter transient EBADMSG; keeping previous "
                     "filter");
        }
        else
        {
            LOG_WARN("[BLUEZ][central] SetDiscoveryFilter failed: %s",
                     err.message ? err.message : strerror(-r));
        }
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

// ======================================================================
// Function: BluezTransport::central_connect
// - In: bus_mu locked, uses adapter_path / dev_path
// - Out: returns true after Connect is sent, sets connect_inflight
// - Note: starts the connection to the target device
// ======================================================================
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
            (void)adapter_stop_discovery_locked(impl_->bus, impl_->adapter_path,
                                                impl_->discovery_on);
        }

        // Always submit Connect()
        int r = sd_bus_call_method_async(impl_->bus, &impl_->connect_call_slot, "org.bluez",
                                         dev_path().c_str(), "org.bluez.Device1", "Connect",
                                         bluez_on_connect_reply, this, "");
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

// ======================================================================
// Function: BluezTransport::central_start_discovery
// - In: bus_mu locked, adapter_path valid
// - Out: returns true if discovery turns on
// - Note: safe to call repeatedly, only starts when off
// ======================================================================
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

// ======================================================================
// Function: BluezTransport::central_find_gatt_paths
// - In: bus_mu locked, reads the ObjectManager cache
// - Out: true when peer_svc_path, peer_tx_path, and peer_rx_path are all set
// - Note: stops scanning the cache once all three are found
// ======================================================================
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

done_scan:
    if (reply)
        sd_bus_message_unref(reply);
    sd_bus_error_free(&err);

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

// ======================================================================
// Function: BluezTransport::central_enable_notify
// - In: takes bus_mu lock and uses peer_tx_path
// - Out: true if StartNotify succeeds, sets subscribed to true
// - Note: stops discovery if it was running
// ======================================================================
bool BluezTransport::central_enable_notify()
{
#if !BITCHAT_HAVE_SDBUS
    return false;
#else
    if (!impl_ || !impl_->bus || impl_->peer_tx_path.empty())
        return false;

    std::lock_guard<std::mutex> lk(impl_->bus_mu);
    bool ok = char_start_notify_locked(impl_->bus, impl_->peer_tx_path, impl_->subscribed);
    return ok;
#endif
}

// ======================================================================
// Function: BluezTransport::central_cold_scan
// - In: bus_mu locked, calls GetManagedObjects
// - Out: true if the walk succeeds, may set dev_path from cache
// - Note: does not start active discovery
// ======================================================================
bool BluezTransport::central_cold_scan(bool refresh_only)
{
#if !BITCHAT_HAVE_SDBUS
    return false;
#else
    sd_bus_message *reply = nullptr;
    sd_bus_error err{};
    int r = 0;
    {
        std::lock_guard<std::mutex> lk(impl_->bus_mu);
        if (!impl_->bus)
            return false;
        r = sd_bus_call_method(impl_->bus, "org.bluez", "/", "org.freedesktop.DBus.ObjectManager",
                               "GetManagedObjects", &err, &reply, "");
    }

    if (r < 0)
    {
        if (-r == EBADMSG)
        {
            LOG_INFO("[BLUEZ][central] GetManagedObjects transient EBADMSG; will retry");
        }
        else
        {
            LOG_WARN("[BLUEZ][central] GetManagedObjects failed: %s",
                     err.message ? err.message : strerror(-r));
        }

        sd_bus_error_free(&err);
        if (reply)
            sd_bus_message_unref(reply);
        return false;
    }

    const std::string dev_prefix = "/org/bluez/" + cfg_.adapter + "/dev_";
    const std::string &want_uuid = cfg_.svc_uuid;

    // collect results locally
    struct LocalDev
    {
        std::string path, addr;
        int16_t rssi;
        bool have_rssi;
        bool svc_hit;
    };
    std::vector<LocalDev> found;
    found.reserve(8);

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
        bool device_svc_hit = false;
        std::string device_addr;
        int16_t device_rssi = 0;
        bool device_have_rssi = false;

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
                        device_svc_hit |= hit;
                    }
                    else if (key && std::strcmp(key, "Address") == 0)
                    {
                        if ((r = read_var_s(reply, device_addr)) < 0)
                            goto out;
                    }
                    else if (key && std::strcmp(key, "RSSI") == 0)
                    {
                        if ((r = read_var_i16(reply, device_rssi)) < 0)
                            goto out;
                        device_have_rssi = true;
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

        // Buffer all device entries; filter/adopt after we re-acquire the lock
        found.push_back(LocalDev{path, device_addr, device_rssi, device_have_rssi,
                                 device_svc_hit});
    }
    if (r < 0)
        goto out;

    // Re-acquire lock to update cache and maybe adopt dev_path
    {
        std::lock_guard<std::mutex> lk(impl_->bus_mu);
        bool has_peer = (cfg_.peer_addr && !cfg_.peer_addr->empty());
        bool adopted = false;
        for (const auto &d : found)
        {
            bool candidate_ok = false;
            if (has_peer)
            {
                bool ok = (!d.addr.empty() && mac_eq(d.addr, *cfg_.peer_addr));
                if (!ok)
                    ok = path_mac_eq(d.path, *cfg_.peer_addr);
                candidate_ok = ok;  // strict: if peer is set, must match it
            }
            else
            {
                candidate_ok = d.svc_hit;
            }
            if (!candidate_ok)
                continue;
            if (!d.addr.empty())
            {
                // write candidates cache
                note_candidate(d.addr, d.have_rssi ? d.rssi : 0);
            }
            if (has_peer && !refresh_only && !adopted && dev_path().empty())
            {
                set_dev_path(d.path.c_str());
                adopted = true;
                if (d.have_rssi)
                    LOG_SYSTEM("[BLUEZ][central] cold-scan found %s addr=%s rssi=%d (svc hit)",
                               d.path.c_str(), d.addr.empty() ? "?" : d.addr.c_str(), (int)d.rssi);
                else
                    LOG_SYSTEM("[BLUEZ][central] cold-scan found %s addr=%s (svc hit)",
                               d.path.c_str(), d.addr.empty() ? "?" : d.addr.c_str());
            }
        }
    }

out:
    if (reply)
        sd_bus_message_unref(reply);
    sd_bus_error_free(&err);
    return r >= 0;
#endif
}

// ======================================================================
// Function: BluezTransport::central_write
// - In: bus_mu locked. Must be connected and subscribed. peer_rx_path and data/len are all valid
// - Out: true if WriteValue succeeds on DBus
// ======================================================================
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

// ======================================================================
// Function: BluezTransport::central_pump
// - In: bus_mu locked, checks connection discovery and subscription
// - Out: may start or stop discovery and initiate connect, discover, notify...
// - Note: stops scanning once subscribed and can restart when needed
// ======================================================================
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
        using namespace std::chrono;
        const uint64_t now_ms =
            (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        bool do_scan = false;
        {
            std::lock_guard<std::mutex> lk(impl_->bus_mu);
            if (now_ms - impl_->last_refresh_ms >= impl_->refresh_min_interval_ms)
            {
                do_scan = true;
                impl_->last_refresh_ms = now_ms;
            }
        }
        if (do_scan)
            (void)central_cold_scan();
    }

    // connect once when device is known, but only if peer_addr is set
    const bool want_connect = (cfg_.peer_addr && !cfg_.peer_addr->empty());
    if (want_connect && !dev_path().empty() && !connected() && !impl_->connect_inflight.load())
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
            // too many noises
            // LOG_DEBUG("[BLUEZ][central] Connected, GATT not exported yet. Waiting...");
        }
    }

    // Discovery policy:
    //   - OFF while a connection attempt is in-flight (some controllers abort if scanning)
    //   - ON otherwise (so we can enumerate peers / handover quickly)
    if (impl_->bus)
    {
        std::lock_guard<std::mutex> lk(impl_->bus_mu);
        if (impl_->connect_inflight.load())
        {
            if (impl_->discovery_on.load())
            {
                (void)adapter_stop_discovery_locked(impl_->bus, impl_->adapter_path,
                                                    impl_->discovery_on);
            }
        }
        else
        {
            if (!impl_->discovery_on.load())
            {
                (void)adapter_start_discovery_locked(impl_->bus, impl_->adapter_path,
                                                     impl_->discovery_on);
            }
        }
    }

    // Async candidates refresh on bus thread
    using namespace std::chrono;
    const uint64_t now_ms =
        (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    bool do_refresh = impl_->refresh_req.exchange(false, std::memory_order_acq_rel);
    uint64_t last_ms = 0;
    uint32_t periodic_ms = 0;
    {
        std::lock_guard<std::mutex> lk(impl_->bus_mu);
        last_ms = impl_->last_refresh_ms;
        periodic_ms = impl_->refresh_periodic_ms;
        if (!do_refresh && (now_ms - last_ms >= periodic_ms))
        {
            do_refresh = true;
        }
    }
    if (do_refresh && impl_->bus)
    {
        (void)central_cold_scan(true);  // refresh cache only
        std::lock_guard<std::mutex> lk(impl_->bus_mu);
        impl_->last_refresh_ms = now_ms;
    }

#endif
}

// ======================================================================
// Function: BluezTransport::central_discover_services
// - In: bus_mu locked and dev_path is set
// - Out: returns true after sending Device1.DiscoverServices
// - Note: asks BlueZ to resolve GATT services. completion is seen via ServicesResolved
// ======================================================================
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

// ======================================================================
// Function: BluezTransport::central_write_frame
// - In: frame length may exceed mtu_payload (we log and still send)
// - Out: true if write path succeeds
// - Note: thin wrapper over central_write and optional TX pause
// ======================================================================
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

// ======================================================================
// Function: BluezTransport::send_central_impl
// - In: data and len valid, central path active
// - Out: true if the payload is written to peer RX
// - Note: thin wrapper around central_write
// ======================================================================
bool BluezTransport::send_central_impl(const Frame &f)
{
    return central_write_frame(f);
}

// ======================================================================
// Function: BluezTransport::start_central
// - In: prepared config and clean initial state
// - Out: sets up matches and may begin discovery
// - Note: spawns the bus loop thread
// ======================================================================
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
                            bluez_on_iface_added, this);
    if (r < 0)
    {
        LOG_ERROR("[BLUEZ][central] subscribe to InterfacesAdded failed: %d", r);
        return false;
    }
    r = sd_bus_match_signal(impl_->bus, &impl_->removed_slot, "org.bluez", "/",
                            "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved",
                            bluez_on_iface_removed, this);
    if (r < 0)
    {
        LOG_ERROR("[BLUEZ][central] subscribe to InterfacesRemoved failed: %d", r);
        return false;
    }
    // PropertiesChanged (Device1.ServicesResolved / GattCharacteristic1.Value etc.)
    r = sd_bus_match_signal(impl_->bus, &impl_->props_slot, "org.bluez", "/",
                            "org.freedesktop.DBus.Properties", "PropertiesChanged",
                            bluez_on_props_changed, this);
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
            }
            // do not hold the lock while waiting, otherwise the cli will deadlock
            {
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

// ======================================================================
// Function: BluezTransport::stop_central
// - In: may be called anytime, will take bus_mu as needed
// - Out: tears down matches and threads, discovery is turned off
// - Note: joins the bus loop thread outside of locks
// ======================================================================
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

// ======================================================================
// Function: BluezTransport::handover_to
// - In: addr in AA:BB:CC:DD:EE:FF format
// - Out: stops current connection and starts connecting to addr
// - Note: the actual connect is done in central_pump()
// ======================================================================
bool BluezTransport::handover_to(const std::string &addr)
{
#if !BITCHAT_HAVE_SDBUS
    (void)addr;
    return false;
#else
    if (!impl_ || !impl_->bus)
        return false;

    LOG_DEBUG("[BLUEZ][handover] to %s...", addr.c_str());

    {
        std::lock_guard<std::mutex> lk(impl_->bus_mu);

        // stop discovery if running
        if (impl_->discovery_on.load())
        {
            (void)adapter_stop_discovery_locked(impl_->bus, impl_->adapter_path,
                                                impl_->discovery_on);
        }

        // if having an in-flight connection attempt, cancel its callback to avoid interference
        if (impl_->connect_inflight.load())
        {
            unref_slot(impl_->connect_call_slot);
            impl_->connect_inflight.store(false);
        }

        // disconnect old device (best-effort)
        if (!impl_->dev_path.empty())
        {
            sd_bus_error derr = SD_BUS_ERROR_NULL;
            sd_bus_message *drep = nullptr;
            (void)sd_bus_call_method(impl_->bus, "org.bluez", impl_->dev_path.c_str(),
                                     "org.bluez.Device1", "Disconnect", &derr, &drep, "");
            if (drep)
                sd_bus_message_unref(drep);
            sd_bus_error_free(&derr);

            // sd_bus_error rerr = SD_BUS_ERROR_NULL;
            // sd_bus_message *rrep = nullptr;
            // (void)sd_bus_call_method(impl_->bus, "org.bluez", impl_->adapter_path.c_str(),
            //                          "org.bluez.Adapter1", "RemoveDevice",
            //                          &rerr, &rrep, "o", impl_->dev_path.c_str());
            // if (rrep) sd_bus_message_unref(rrep);
            // sd_bus_error_free(&rerr);
        }

        // clear state
        impl_->peer_svc_path.clear();
        impl_->peer_tx_path.clear();
        impl_->peer_rx_path.clear();
        impl_->dev_path.clear();
        impl_->connected.store(false);
        impl_->subscribed.store(false);
        impl_->services_resolved.store(false);
        impl_->discover_submitted.store(false);

        // backoff, sleep 300ms and then connect
        using namespace std::chrono;
        const uint64_t now_ms =
            (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        impl_->next_connect_at_ms = now_ms + 300;

        // Set new target peer
        cfg_.peer_addr = addr;
        // requrire a cold scan for the next round
        impl_->refresh_req.store(true, std::memory_order_release);
        impl_->last_refresh_ms = 0;
    }

    // set filter and restart discovery immediately
    (void)central_set_discovery_filter();
    (void)central_start_discovery();

    LOG_SYSTEM("[BLUEZ][handover] target=%s", addr.c_str());
    return true;
#endif
}

}  // namespace transport