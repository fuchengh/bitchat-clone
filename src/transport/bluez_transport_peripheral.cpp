/* ======================================================================
 * BlueZ Peripheral — normal path
 *
 *  App thread                       Bus thread                 BlueZ/DBus           Peer (Central)
 *  ----------                       -----------                -----------          --------------
 *  start_peripheral()
 *    └─ export Service / TX / RX
 *    └─ register advertisement ──────────────────────────────────────────────────▶  LEAdvertisingManager1.RegisterAdvertisement
 *    └─ spawn bus loop
 *
 *                                   ◀──────────────────────── Connect from central
 *                                   ◀───── StartNotify on TX ─────────────────────  GattCharacteristic1.StartNotify
 *                                      └─ set Notifying=true
 *                                      └─ emit PropertiesChanged(Notifying)
 *
 *  Data
 *  send_peripheral(data)
 *    └─ emit PropertiesChanged(Value=ay on TX) ──────────────────────────────────▶  notify central
 *
 *                                   ◀────── WriteValue on RX ────────────────────  GattCharacteristic1.WriteValue
 *                                      └─ deliver_rx_bytes to app
 *
 *  Teardown
 *  stop_peripheral()
 *    └─ unexport objects / unregister ADV ───────────────────────────────────────▶  cleanup
 *
 *  Ready condition: tx_notifying() == true
 *  DBus calls on app thread are under impl_->bus_mu
 * ====================================================================== */

#include <cstring>
#include <mutex>

// clang-format off
#include "transport/bluez_transport.hpp"
#include "transport/bluez_transport_impl.hpp"
#include "util/log.hpp"
#include "transport/bluez_helper_peripheral.hpp"
// clang-format on

#if BITCHAT_HAVE_SDBUS
#include <poll.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

namespace
{

// ======================================================================
// Function: emit_value_props_changed_ay
// - In: bus and tx_path valid, data and len are the payload
// - Out: sends PropertiesChanged with Value=ay on the TX characteristic
// - Note: fast path for peripheral notify without touching Notifying
// ======================================================================
static bool emit_value_props_changed_ay(sd_bus            *bus,
                                        const std::string &tx_path,
                                        const uint8_t     *data,
                                        size_t             len)
{
    if (!bus || !data || len == 0)
        return false;
    sd_bus_message *sig = nullptr;
    int             r   = sd_bus_message_new_signal(bus, &sig, tx_path.c_str(),
                                                    "org.freedesktop.DBus.Properties", "PropertiesChanged");
    // clang-format off
    if (r < 0) goto fail_new;
    // interface
    r = sd_bus_message_append(sig, "s", "org.bluez.GattCharacteristic1");
    if (r < 0) goto fail;
    // a{sv}
    r = sd_bus_message_open_container(sig, SD_BUS_TYPE_ARRAY, "{sv}");
    if (r < 0) goto fail;
    r = sd_bus_message_open_container(sig, SD_BUS_TYPE_DICT_ENTRY, "sv");
    if (r < 0) goto fail;
    r = sd_bus_message_append(sig, "s", "Value");
    if (r < 0) goto fail;
    r = sd_bus_message_open_container(sig, SD_BUS_TYPE_VARIANT, "ay");
    if (r < 0) goto fail;
    r = sd_bus_message_append_array(sig, 'y', data, len);
    if (r < 0) goto fail;
    r = sd_bus_message_close_container(sig); /* variant */
    if (r < 0) goto fail;
    r = sd_bus_message_close_container(sig); /* dict entry */
    if (r < 0) goto fail;
    r = sd_bus_message_close_container(sig); /* a{sv} */
    if (r < 0) goto fail;
    // invalidated props: empty 'as'
    r = sd_bus_message_open_container(sig, SD_BUS_TYPE_ARRAY, "s");
    if (r < 0) goto fail;
    r = sd_bus_message_close_container(sig);
    if (r < 0) goto fail;
    // send
    r = sd_bus_send(bus, sig, nullptr);
    sd_bus_message_unref(sig);
    return (r >= 0);
    // clang-format off
fail:
    if (sig)
        sd_bus_message_unref(sig);
fail_new:
    return false;
}
}  // namespace
#endif

namespace transport
{

// ======================================================================
// Function: BluezTransport::emit_tx_props_changed
// - In: bus valid and tx_path set, prop is a GATT property name like Notifying
// - Out: emits PropertiesChanged on the TX characteristic
// - Note: used when StartNotify or StopNotify flips Notifying
// ======================================================================
void BluezTransport::emit_tx_props_changed(const char *prop)
{
#if BITCHAT_HAVE_SDBUS
    if (!impl_ || !impl_->bus)
        return;
    sd_bus_emit_properties_changed(impl_->bus, impl_->tx_path.c_str(),
                                   "org.bluez.GattCharacteristic1", prop, nullptr);
#else
    (void)prop;
#endif
}

// ======================================================================
// Function: BluezTransport::start_peripheral
// - In: prepared config and clean initial state
// - Out: exports service and characteristics and starts advertising
// - Note: spawns the bus loop thread
// ======================================================================
bool BluezTransport::start_peripheral()
{
    // ======================================================================
    // Export order
    // - ObjectManager (app) -> GattService1 -> TX/RX -> LEAdvertisement1
    // - Finally register advertisement
    // - Vtable definitions are in bluez_helper_peripheral.cpp
    // ======================================================================

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
                                 "org.bluez.GattService1", gatt_service_vtable, /*userdata=*/this);
    if (r < 0)
    {
        LOG_ERROR("[BLUEZ][peripheral] add service vtable failed: %d", r);
        return false;
    }
    LOG_DEBUG("[BLUEZ][peripheral] service vtable exported at %s (bus=%s)",
              impl_->svc_path.c_str(), impl_->unique_name.c_str());
    // TX characteristic: org.bluez.GattCharacteristic1 (Flags=["notify"])
    r = sd_bus_add_object_vtable(impl_->bus, &impl_->tx_slot, impl_->tx_path.c_str(),
                                 "org.bluez.GattCharacteristic1", gatt_tx_vtable, /*userdata=*/this);
    if (r < 0)
    {
        LOG_ERROR("[BLUEZ][peripheral] add TX vtable failed: %d", r);
        return false;
    }
    // RX characteristic: org.bluez.GattCharacteristic1 (Flags=["write"])
    r = sd_bus_add_object_vtable(impl_->bus, &impl_->rx_slot, impl_->rx_path.c_str(),
                                 "org.bluez.GattCharacteristic1", gatt_rx_vtable, /*userdata=*/this);
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
            {
                std::lock_guard<std::mutex> lk(impl_->bus_mu);
                while (1)
                {
                    int pr = sd_bus_process(impl_->bus, nullptr);
                    if (pr <= 0)
                        break;
                }
                const uint64_t WAIT_USEC = 100000;  // 100ms
                sd_bus_wait(impl_->bus, WAIT_USEC);
            }
        }
    });

    return true;
#endif
}

// ======================================================================
// Function: BluezTransport::stop_peripheral
// - In: may be called anytime, will take bus_mu as needed
// - Out: unexports objects and clears all slots, stops advertisement
// - Note: joins the bus loop thread outside of locks
// ======================================================================
void BluezTransport::stop_peripheral()
{
    // clang-format off
#if BITCHAT_HAVE_SDBUS
    // unreg before closing the bus
    if (impl_->bus)
    {
        std::lock_guard<std::mutex> lk(impl_->bus_mu);
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
        // wake the sd-bus loop if blocked in sd_bus_wait
        sd_bus_close(impl_->bus);
    }

    if (impl_->loop.joinable())
        impl_->loop.join();

    // cleanup
    unref_slot(impl_->reg_slot);
    unref_slot(impl_->rx_slot);
    unref_slot(impl_->tx_slot);
    unref_slot(impl_->svc_slot);
    unref_slot(impl_->app_slot);
    unref_slot(impl_->adv_call_slot);
    unref_slot(impl_->adv_obj_slot);

    if (impl_->bus) {
        sd_bus_flush_close_unref(impl_->bus);
        impl_->bus = nullptr;
    }
// clang-format on
#endif
}

bool BluezTransport::peripheral_can_notify()
{
#if BITCHAT_HAVE_SDBUS
    if (!impl_ || !bus())
    {
        return false;
    }
    if (!tx_notifying())
    {
        LOG_DEBUG("[BLUEZ][peripheral] drop send (Notifying=false)");
        return false;
    }
    return true;
#else
    return false;
#endif
}

// ======================================================================
// Function: BluezTransport::send_peripheral_impl
// - In: tx notifications are on, data/len are valid
// - Out: true if a Value update is emitted on the TX characteristic
// - Note: used to notify the central
// ======================================================================
bool BluezTransport::send_peripheral_impl(const Frame &f)
{
#if !BITCHAT_HAVE_SDBUS
    (void)f;
    return false;
#else
    // Locked path: we may be called outside the bus thread; use bus_mu for sd-bus calls.
    if (!peripheral_can_notify())
        return false;
    const uint8_t *data = f.data();
    const size_t   len  = f.size();
    if (!data || len == 0)
        return false;
    std::lock_guard<std::mutex> lk(impl_->bus_mu);
    bool ok = emit_value_props_changed_ay(impl_->bus, impl_->tx_path, data, len);
    if (!ok)
        LOG_WARN("[BLUEZ][peripheral] notify(send, locked) failed");
    return ok;
#endif
}

}  // namespace transport
