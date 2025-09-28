// =============================================================================
// PERIPHERAL ROLE IMPLEMENTATION
// GATT/ADV object export / StartNotify/StopNotify / WriteValue / notify fast-path
//
// READY SIGNAL:
//  - link_ready() uses Impl::notifying, toggled by StartNotify/StopNotify cbs
// ============================================================================

#include <cstring>
#include <mutex>

// clang-format off
#include "transport/bluez_transport.hpp"
#include "transport/bluez_transport_impl.hpp"
#include "util/log.hpp"
#include "transport/bluez_dbus_util.hpp"
// clang-format on

#if BITCHAT_HAVE_SDBUS
#include <poll.h>
#include <systemd/sd-bus.h>
#include <unistd.h>
#include "bluez_helper.inc"

// clang-format off
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
    SD_BUS_PROPERTY("IncludeTxPower", "b", adv_prop_IncludeTxPower, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_METHOD("Release", "", "", adv_Release, SD_BUS_VTABLE_UNPRIVILEGED), SD_BUS_VTABLE_END};
#endif
// clang-format on

#if BITCHAT_HAVE_SDBUS
namespace
{
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

// ===== Bus-thread fast path notify =====
bool BluezTransport::peripheral_notify_ay_from_bus_thread(const uint8_t *data, size_t len)
{
#if !BITCHAT_HAVE_SDBUS
    (void)data;
    (void)len;
    return false;
#else
    // Called from sd-bus callback thread; DO NOT take bus_mu here.
    // Sends PropertiesChanged("Value"=ay) on TX characteristic.
    if (!impl_ || !impl_->bus)
        return false;
    if (!tx_notifying())
        return false;  // central hasn't StartNotify yet
    bool ok = emit_value_props_changed_ay(impl_->bus, impl_->tx_path, data, len);
    if (!ok)
        LOG_WARN("[BLUEZ][peripheral] notify(send) failed");
    else
        LOG_DEBUG("[BLUEZ][peripheral] notify(len=%zu) sent (bus-thread)", len);
    return ok;
#endif
}

void BluezTransport::emit_tx_props_changed(const char *prop)
{
    if (!impl_ || !impl_->bus)
        return;
    sd_bus_emit_properties_changed(impl_->bus, impl_->tx_path.c_str(),
                                   "org.bluez.GattCharacteristic1", prop, nullptr);
}

bool BluezTransport::start_peripheral()
{
    // Export order: ObjectManager (app) -> GattService1 -> TX/RX -> LEAdvertisement1 -> RegisterAdvertisement

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
