// src/transport/bluez_helper_peripheral.cpp
#include "transport/bluez_helper_peripheral.hpp"
#include "transport/bluez_transport.hpp"

#if BITCHAT_HAVE_SDBUS
#include <poll.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

// GattService1 (svc_path)
const sd_bus_vtable gatt_service_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("UUID", "s", svc_prop_UUID, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Primary", "b", svc_prop_Primary, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Includes", "ao", svc_prop_Includes, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END};

// TX char (notify) + Start/StopNotify
const sd_bus_vtable gatt_tx_vtable[] = {
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
const sd_bus_vtable gatt_rx_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("UUID", "s", chr_prop_UUID, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Service", "o", chr_prop_Service, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Flags", "as", chr_prop_Flags, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_METHOD("WriteValue", "aya{sv}", "", rx_WriteValue, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END};

// LEAdvertisement1
// Note: immutable properties plus Release method
const sd_bus_vtable adv_vtable[] = {
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

// Peripheral DBus callbacks
int on_reg_app_reply(sd_bus_message *m, void *userdata, sd_bus_error * /*ret_error*/)
{
    auto *self = static_cast<transport::BluezTransport *>(userdata);
    if (sd_bus_message_is_method_error(m, nullptr))
    {
        const sd_bus_error *e = sd_bus_message_get_error(m);
        LOG_ERROR("[BLUEZ][peripheral] RegisterApplication failed: %s: %s",
                  e && e->name ? e->name : "unknown", e && e->message ? e->message : "no message");
    }
    else
    {
        LOG_DEBUG("[BLUEZ][peripheral] GATT app registered at %s (bus=%s)",
                  self->app_path().c_str(), self->unique_name().c_str());
    }
    return 1;
}

// ======================================================================
// Function: tx_StartNotify
// - In: method call on TX characteristic
// - Out: turns on notifying and replies success
// - Note: emits PropertiesChanged for Notifying
// ======================================================================
int tx_StartNotify(sd_bus_message *m, void *userdata, sd_bus_error * /*ret_err*/)
{
    auto *self = static_cast<transport::BluezTransport *>(userdata);
    self->set_tx_notifying(true);
    self->emit_tx_props_changed("Notifying");
    LOG_DEBUG("[BLUEZ][peripheral] tx.StartNotify");
    return sd_bus_reply_method_return(m, "");
}

// ======================================================================
// Function: tx_StopNotify
// - In: method call on TX characteristic
// - Out: turns off notifying and replies success
// - Note: emits PropertiesChanged for Notifying
// ======================================================================
int tx_StopNotify(sd_bus_message *m, void *userdata, sd_bus_error * /*ret_err*/)
{
    auto *self = static_cast<transport::BluezTransport *>(userdata);
    self->set_tx_notifying(false);
    self->emit_tx_props_changed("Notifying");
    LOG_DEBUG("[BLUEZ][peripheral] tx.StopNotify");
    return sd_bus_reply_method_return(m, "");
}

// ======================================================================
// Function: rx_WriteValue
// - In: method call on RX characteristic with payload and options
// - Out: delivers bytes to transport and replies success
// - Note: rejects non-zero offset
// ======================================================================
int rx_WriteValue(sd_bus_message *m, void *userdata, sd_bus_error * /*ret_err*/)
{
    auto       *self = static_cast<transport::BluezTransport *>(userdata);
    const void *buf  = nullptr;
    size_t      len  = 0;

    int r = sd_bus_message_read_array(m, 'y', &buf, &len);
    if (r < 0)
        return r;
    LOG_DEBUG("[BLUEZ][peripheral] rx.WriteValue len=%zu", len);

    uint16_t offset = 0;

    r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "{sv}");
    if (r < 0)
        return r;
    while ((r = sd_bus_message_enter_container(m, SD_BUS_TYPE_DICT_ENTRY, "sv")) > 0)
    {
        const char *key = nullptr;
        if ((r = sd_bus_message_read(m, "s", &key)) < 0)
            return r;

        if (key && std::strcmp(key, "offset") == 0)
        {
            if ((r = sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, "q")) < 0)
                return r;
            uint16_t off = 0;
            if ((r = sd_bus_message_read(m, "q", &off)) < 0)
                return r;
            offset = off;
            if ((r = sd_bus_message_exit_container(m)) < 0)
                return r;
        }
        else
        {
            if ((r = sd_bus_message_skip(m, "v")) < 0)
                return r;
        }
        if ((r = sd_bus_message_exit_container(m)) < 0)
            return r;
    }
    if (r < 0)
        return r;
    if ((r = sd_bus_message_exit_container(m)) < 0)
        return r;

    if (offset != 0)
        return sd_bus_reply_method_errorf(m, "org.bluez.Error.InvalidOffset",
                                          "Offset %u not supported", offset);

    if (self && buf && len != 0)
        self->deliver_rx_bytes(static_cast<const uint8_t *>(buf), len);

    return sd_bus_reply_method_return(m, "");
}

// ======================================================================
// Function: adv_Release
// - In: method call on advertisement object
// - Out: replies success
// - Note: used by BlueZ when unregistering the advertiser
// ======================================================================
int adv_Release(sd_bus_message *m, void * /*userdata*/, sd_bus_error * /*ret*/)
{
    LOG_DEBUG("[BLUEZ][peripheral] adv.Release()");
    return sd_bus_reply_method_return(m, "");
}

int on_reg_adv_reply(sd_bus_message *m, void * /*userdata*/, sd_bus_error * /*ret*/)
{
    if (sd_bus_message_is_method_error(m, nullptr))
    {
        const sd_bus_error *e = sd_bus_message_get_error(m);
        LOG_ERROR("[BLUEZ][peripheral] RegisterAdvertisement failed: %s: %s",
                  e && e->name ? e->name : "unknown", e && e->message ? e->message : "no message");
    }
    else
    {
        LOG_SYSTEM("[BLUEZ][peripheral] LE advertisement registered successfully");
    }
    return 1;
}

// ======================================================================
// Function: svc_prop_UUID
// - In: reply and userdata provided by sd-bus property get
// - Out: appends service UUID string
// - Note: part of org.bluez.GattService1
// ======================================================================
int svc_prop_UUID(sd_bus * /*bus*/,
                  const char * /*path*/,
                  const char * /*iface*/,
                  const char * /*prop*/,
                  sd_bus_message *reply,
                  void           *userdata,
                  sd_bus_error * /*ret_err*/)
{
    auto *self = static_cast<transport::BluezTransport *>(userdata);
    return sd_bus_message_append(reply, "s", self->config().svc_uuid.c_str());
}

// ======================================================================
// Function: svc_prop_Primary
// - In: property get on the service
// - Out: appends true to indicate primary service
// - Note: constant result
// ======================================================================
int svc_prop_Primary(sd_bus * /*bus*/,
                     const char * /*path*/,
                     const char * /*iface*/,
                     const char * /*prop*/,
                     sd_bus_message *reply,
                     void * /*userdata*/,
                     sd_bus_error * /*ret_err*/)
{
    return sd_bus_message_append(reply, "b", 1);
}

// ======================================================================
// Function: svc_prop_Includes
// - In: property get on the service
// - Out: appends empty array of object paths
// - Note: we do not include other services
// ======================================================================
int svc_prop_Includes(sd_bus * /*bus*/,
                      const char * /*path*/,
                      const char * /*iface*/,
                      const char * /*prop*/,
                      sd_bus_message *reply,
                      void * /*userdata*/,
                      sd_bus_error * /*ret_err*/)
{
    int r = sd_bus_message_open_container(reply, 'a', "o");
    if (r < 0)
        return r;
    return sd_bus_message_close_container(reply);
}

// ======================================================================
// Function: chr_prop_UUID
// - In: property get on a characteristic path
// - Out: appends TX or RX UUID based on the path
// - Note: shared handler for both characteristics
// ======================================================================
int chr_prop_UUID(sd_bus * /*bus*/,
                  const char *path,
                  const char * /*iface*/,
                  const char * /*prop*/,
                  sd_bus_message *reply,
                  void           *userdata,
                  sd_bus_error * /*ret_err*/)
{
    auto       *self = static_cast<transport::BluezTransport *>(userdata);
    std::string p(path);
    if (p == self->tx_path())
        return sd_bus_message_append(reply, "s", self->config().tx_uuid.c_str());
    if (p == self->rx_path())
        return sd_bus_message_append(reply, "s", self->config().rx_uuid.c_str());
    return -EINVAL;
}

// ======================================================================
// Function: chr_prop_Service
// - In: property get on a characteristic
// - Out: appends the owning service object path
// - Note: ties char back to the service
// ======================================================================
int chr_prop_Service(sd_bus * /*bus*/,
                     const char * /*path*/,
                     const char * /*iface*/,
                     const char * /*prop*/,
                     sd_bus_message *reply,
                     void           *userdata,
                     sd_bus_error * /*ret_err*/)
{
    auto *self = static_cast<transport::BluezTransport *>(userdata);
    return sd_bus_message_append(reply, "o", self->svc_path().c_str());
}

// ======================================================================
// Function: chr_prop_Flags
// - In: property get on a characteristic path
// - Out: appends allowed flags for TX or RX
// - Note: TX is notify, RX is write and write-without-response
// ======================================================================
int chr_prop_Flags(sd_bus * /*bus*/,
                   const char *path,
                   const char * /*iface*/,
                   const char * /*prop*/,
                   sd_bus_message *reply,
                   void           *userdata,
                   sd_bus_error * /*ret_err*/)
{
    auto       *self = static_cast<transport::BluezTransport *>(userdata);
    std::string p(path);
    int         r;

    if (p == self->tx_path())
    {
        r = sd_bus_message_open_container(reply, 'a', "s");
        if (r < 0)
            return r;
        r = sd_bus_message_append_basic(reply, 's', "notify");
        if (r < 0)
            return r;
        return sd_bus_message_close_container(reply);
    }
    else if (p == self->rx_path())
    {
        // RX characteristic: write + write-without-response
        r = sd_bus_message_open_container(reply, 'a', "s");
        if (r < 0)
            return r;
        // RX characteristic: write + write-without-response (enable ATT Write Command)
        r = sd_bus_message_append(reply, "s", "write");
        if (r < 0)
            return r;
        r = sd_bus_message_append(reply, "s", "write-without-response");
        if (r < 0)
            return r;
        return sd_bus_message_close_container(reply);
    }
    return -EINVAL;
}

// ======================================================================
// Function: chr_prop_Notifying
// - In: property get on TX characteristic
// - Out: appends current Notifying state
// - Note: only meaningful for TX
// ======================================================================
int chr_prop_Notifying(sd_bus * /*bus*/,
                       const char *path,
                       const char * /*iface*/,
                       const char * /*prop*/,
                       sd_bus_message *reply,
                       void           *userdata,
                       sd_bus_error * /*ret_err*/)
{
    auto *self = static_cast<transport::BluezTransport *>(userdata);
    int   b    = (std::string(path) == self->tx_path()) ? (self->tx_notifying() ? 1 : 0) : 0;
    return sd_bus_message_append(reply, "b", b);
}

// ======================================================================
// Function: adv_prop_Type
// - In: property get on advertisement
// - Out: appends string peripheral
// - Note: advertisement type
// ======================================================================
int adv_prop_Type(sd_bus * /*bus*/,
                  const char * /*path*/,
                  const char * /*iface*/,
                  const char * /*prop*/,
                  sd_bus_message *reply,
                  void * /*userdata*/,
                  sd_bus_error * /*ret*/)
{
    return sd_bus_message_append(reply, "s", "peripheral");
}

// ======================================================================
// Function: adv_prop_ServiceUUIDs
// - In: property get on advertisement
// - Out: appends one service UUID in an array
// - Note: advertises our primary service
// ======================================================================
int adv_prop_ServiceUUIDs(sd_bus * /*bus*/,
                          const char * /*path*/,
                          const char * /*iface*/,
                          const char * /*prop*/,
                          sd_bus_message *reply,
                          void           *userdata,
                          sd_bus_error * /*ret*/)
{
    auto *self = static_cast<transport::BluezTransport *>(userdata);
    int   r    = sd_bus_message_open_container(reply, 'a', "s");
    if (r < 0)
        return r;
    r = sd_bus_message_append_basic(reply, 's', self->config().svc_uuid.c_str());
    if (r < 0)
        return r;
    return sd_bus_message_close_container(reply);
}

// ======================================================================
// Function: adv_prop_LocalName
// - In: property get on advertisement
// - Out: appends the local name
// - Note: comes from config
// ======================================================================
int adv_prop_LocalName(sd_bus * /*bus*/,
                       const char * /*path*/,
                       const char * /*iface*/,
                       const char * /*prop*/,
                       sd_bus_message *reply,
                       void * /*userdata*/,
                       sd_bus_error * /*ret*/)
{
    return sd_bus_message_append(reply, "s", "BitChat");
}

// ======================================================================
// Function: adv_prop_IncludeTxPower
// - In: property get on advertisement
// - Out: appends true to include Tx power
// - Note: constant choice
// ======================================================================
int adv_prop_IncludeTxPower(sd_bus * /*bus*/,
                            const char * /*path*/,
                            const char * /*iface*/,
                            const char * /*prop*/,
                            sd_bus_message *reply,
                            void * /*userdata*/,
                            sd_bus_error * /*ret*/)
{
    return sd_bus_message_append(reply, "b", 0);
}

#endif