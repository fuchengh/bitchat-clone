#include <errno.h>
#include <string>
#include <thread>
#include <utility>

#include "transport/bluez_transport.hpp"
#include "transport/itransport.hpp"
#include "util/log.hpp"

#if BITCHAT_HAVE_SDBUS
#include <systemd/sd-bus.h>

// --- Registration helper ---
static int on_reg_app_reply(sd_bus_message *m, void *userdata, sd_bus_error * /*ret_error*/)
{
    auto *self = static_cast<transport::BluezTransport *>(userdata);
    // Check if this is an error reply
    if (sd_bus_message_is_method_error(m, nullptr))
    {
        const sd_bus_error *e = sd_bus_message_get_error(m);
        LOG_ERROR("[BLUEZ] RegisterApplication failed: %s: %s", e && e->name ? e->name : "unknown",
                  e && e->message ? e->message : "no message");
        self->set_reg_ok(false);
    }
    else
    {
        LOG_INFO("[BLUEZ] GATT app registered at %s (bus=%s)", self->app_path().c_str(),
                 self->unique_name().c_str());
        self->set_reg_ok(true);
    }
    return 1;  // handled
}

// --- Service (org.bluez.GattService1) property getters ---
static int svc_prop_UUID(sd_bus * /*bus*/,
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

static int svc_prop_Primary(sd_bus * /*bus*/,
                            const char * /*path*/,
                            const char * /*iface*/,
                            const char * /*prop*/,
                            sd_bus_message *reply,
                            void * /*userdata*/,
                            sd_bus_error * /*ret_err*/)
{
    return sd_bus_message_append(reply, "b", 1);  // append bool true
}

static int svc_prop_Includes(sd_bus * /*bus*/,
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

static int chr_prop_UUID(sd_bus * /*bus*/,
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

static int chr_prop_Service(sd_bus * /*bus*/,
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

static int chr_prop_Flags(sd_bus * /*bus*/,
                          const char *path,
                          const char * /*iface*/,
                          const char * /*prop*/,
                          sd_bus_message *reply,
                          void           *userdata,
                          sd_bus_error * /*ret_err*/)
{
    auto       *self = static_cast<transport::BluezTransport *>(userdata);
    std::string p(path);
    const char *flag = nullptr;
    if (p == self->tx_path())
        flag = "notify";
    else if (p == self->rx_path())
        flag = "write";
    else
        return -EINVAL;

    int r = sd_bus_message_open_container(reply, 'a', "s");
    if (r < 0)
        return r;
    r = sd_bus_message_append_basic(reply, 's', flag);
    if (r < 0)
        return r;
    return sd_bus_message_close_container(reply);
}

static int chr_prop_Notifying(sd_bus * /*bus*/,
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

// ---- TX Methods ----
static int tx_StartNotify(sd_bus_message *m, void *userdata, sd_bus_error * /*ret_err*/)
{
    auto *self = static_cast<transport::BluezTransport *>(userdata);
    self->set_tx_notifying(true);
    self->emit_tx_props_changed("Notifying");
    return sd_bus_reply_method_return(m, "");
}

static int tx_StopNotify(sd_bus_message *m, void *userdata, sd_bus_error * /*ret_err*/)
{
    auto *self = static_cast<transport::BluezTransport *>(userdata);
    self->set_tx_notifying(false);
    self->emit_tx_props_changed("Notifying");
    return sd_bus_reply_method_return(m, "");
}

// ---- RX Methods ----
static int rx_WriteValue(sd_bus_message *m, void * /*userdata*/, sd_bus_error * /*ret_err*/)
{
    const void *buf = nullptr;
    size_t      len = 0;

    // read "ay"
    int r = sd_bus_message_read_array(m, 'y', &buf, &len);
    if (r < 0)
        return r;
    LOG_INFO("[BLUEZ] rx.WriteValue len=%zu", len);

    // read "a{sv}" (options map) and skip entries
    r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "{sv}");
    if (r < 0)
        return r;

    while ((r = sd_bus_message_at_end(m, 0)) == 0)
    {
        r = sd_bus_message_skip(m, "{sv}");
        if (r < 0)
            return r;
    }
    r = sd_bus_message_exit_container(m);
    if (r < 0)
        return r;

    // empty reply (success)
    return sd_bus_reply_method_return(m, "");
}

// ---- Advertising BitChat App ----
static int adv_Release(sd_bus_message *m, void * /*userdata*/, sd_bus_error * /*ret*/)
{
    LOG_INFO("[BLUEZ] adv.Release()");
    return sd_bus_reply_method_return(m, "");
}

// Properties
static int adv_prop_Type(sd_bus * /*bus*/,
                         const char * /*path*/,
                         const char * /*iface*/,
                         const char * /*prop*/,
                         sd_bus_message *reply,
                         void * /*userdata*/,
                         sd_bus_error * /*ret*/)
{
    return sd_bus_message_append(reply, "s", "peripheral");
}

static int adv_prop_ServiceUUIDs(sd_bus * /*bus*/,
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

static int adv_prop_LocalName(sd_bus * /*bus*/,
                              const char * /*path*/,
                              const char * /*iface*/,
                              const char * /*prop*/,
                              sd_bus_message *reply,
                              void * /*userdata*/,
                              sd_bus_error * /*ret*/)
{
    return sd_bus_message_append(reply, "s", "BitChat");
}

static int adv_prop_IncludeTxPower(sd_bus * /*bus*/,
                                   const char * /*path*/,
                                   const char * /*iface*/,
                                   const char * /*prop*/,
                                   sd_bus_message *reply,
                                   void * /*userdata*/,
                                   sd_bus_error * /*ret*/)
{
    return sd_bus_message_append(reply, "b", 0);  // false for now
}

static int on_reg_adv_reply(sd_bus_message *m, void * /*userdata*/, sd_bus_error * /*ret*/)
{
    if (sd_bus_message_is_method_error(m, nullptr))
    {
        const sd_bus_error *e = sd_bus_message_get_error(m);
        LOG_ERROR("[BLUEZ] RegisterAdvertisement failed: %s: %s",
                  e && e->name ? e->name : "unknown", e && e->message ? e->message : "no message");
    }
    else
    {
        LOG_INFO("[BLUEZ] LE advertisement registered");
    }
    return 1;
}

// --- vtable describing org.bluez.GattService1 on svc_path ---
static const sd_bus_vtable svc_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("UUID", "s", svc_prop_UUID, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Primary", "b", svc_prop_Primary, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Includes", "ao", svc_prop_Includes, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END};

// TX characteristic vtable: UUID/Service/Flags/Notifying + Start/StopNotify
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

// RX characteristic vtable: UUID/Service/Flags + WriteValue
static const sd_bus_vtable rx_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("UUID", "s", chr_prop_UUID, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Service", "o", chr_prop_Service, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Flags", "as", chr_prop_Flags, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_METHOD("WriteValue", "aya{sv}", "", rx_WriteValue, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END};

// LEAdvertisement1 vtable
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
    sd_bus          *bus           = nullptr;
    sd_bus_slot     *app_slot      = nullptr;  // ObjectManager at /com/bitchat/app
    sd_bus_slot     *svc_slot      = nullptr;  // GattService1 at /com/bitchat/app/svc0
    sd_bus_slot     *tx_slot       = nullptr;  // TX characteristic
    sd_bus_slot     *rx_slot       = nullptr;  // RX characteristic
    sd_bus_slot     *adv_obj_slot  = nullptr;  // vtable slot for LEAdvertisement1
    sd_bus_slot     *adv_call_slot = nullptr;  // async RegisterAdvertisement call
    std::string      adv_path      = "/com/bitchat/adv0";
    sd_bus_slot     *reg_slot      = nullptr;
    std::atomic_bool reg_ok{false};
#endif
    std::thread loop;
    // common paths
    std::string adapter_path;  // e.g. "/org/bluez/hci0"
    std::string app_path = "/com/bitchat/app";
    std::string svc_path = "/com/bitchat/app/svc0";
    std::string tx_path  = "/com/bitchat/app/svc0/char_tx";
    std::string rx_path  = "/com/bitchat/app/svc0/char_rx";

    std::atomic_bool notifying{false};  // TX notify state (peripheral)
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
    impl_->reg_ok.store(v);
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

    LOG_INFO("[BLUEZ] stub start: role=%s adapter=%s mtu_payload=%zu svc=%s tx=%s rx=%s%s%s",
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
    // TODO: later we will map Frame -> GATT Write.
    LOG_INFO("[BLUEZ] stub send (frame)");
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

    LOG_INFO("[BLUEZ] stub stop");
    impl_.reset();
}

bool BluezTransport::start_peripheral()
{
#if !BITCHAT_HAVE_SDBUS
    LOG_ERROR("[BLUEZ] sd-bus not available (BITCHAT_HAVE_SDBUS=0)");
    return false;
#else

    int r = (sd_bus_open_system(&impl_->bus));
    if (r < 0 || !impl_->bus)
    {
        LOG_ERROR("[BLUEZ] failed to connect to system bus, err %d", r);
        return false;
    }
    impl_->adapter_path = "/org/bluez/" + cfg_.adapter;
    // not needed, but get a unique name for debugging
    const char *uniq_name = nullptr;
    r                     = sd_bus_get_unique_name(impl_->bus, &uniq_name);
    if (r >= 0 && uniq_name)
        impl_->unique_name = uniq_name;

    r = sd_bus_add_object_manager(impl_->bus, &impl_->app_slot, impl_->app_path.c_str());
    if (r < 0)
    {
        LOG_ERROR("[BLUEZ] add object manager failed: %d", r);
        return false;
    }

    // Service: org.bluez.GattService1 at /com/bitchat/app/svc0
    r = sd_bus_add_object_vtable(impl_->bus, &impl_->svc_slot, impl_->svc_path.c_str(),
                                 "org.bluez.GattService1", svc_vtable, /*userdata=*/this);
    if (r < 0)
    {
        LOG_ERROR("[BLUEZ] add service vtable failed: %d", r);
        return false;
    }
    LOG_INFO("[BLUEZ] service vtable exported at %s (bus=%s)", impl_->svc_path.c_str(),
             impl_->unique_name.c_str());
    // TX characteristic: org.bluez.GattCharacteristic1 (Flags=["notify"])
    r = sd_bus_add_object_vtable(impl_->bus, &impl_->tx_slot, impl_->tx_path.c_str(),
                                 "org.bluez.GattCharacteristic1", tx_vtable, /*userdata=*/this);
    if (r < 0)
    {
        LOG_ERROR("[BLUEZ] add TX vtable failed: %d", r);
        return false;
    }
    // RX characteristic: org.bluez.GattCharacteristic1 (Flags=["write"])
    r = sd_bus_add_object_vtable(impl_->bus, &impl_->rx_slot, impl_->rx_path.c_str(),
                                 "org.bluez.GattCharacteristic1", rx_vtable, /*userdata=*/this);
    if (r < 0)
    {
        LOG_ERROR("[BLUEZ] add RX vtable failed: %d", r);
        return false;
    }
    LOG_INFO("[BLUEZ] char vtables exported: tx=%s rx=%s", impl_->tx_path.c_str(),
             impl_->rx_path.c_str());

    // Register application with BlueZ GattManager1 (async)
    r = sd_bus_call_method_async(impl_->bus, &impl_->reg_slot, "org.bluez",
                                 impl_->adapter_path.c_str(), "org.bluez.GattManager1",
                                 "RegisterApplication", on_reg_app_reply, this, "oa{sv}",
                                 impl_->app_path.c_str(), 0);
    if (r < 0)
    {
        LOG_ERROR("[BLUEZ] RegisterApplication (async) submit failed: %s", strerror(-r));
        return false;
    }

    r = sd_bus_add_object_vtable(impl_->bus, &impl_->adv_obj_slot, impl_->adv_path.c_str(),
                                 "org.bluez.LEAdvertisement1", adv_vtable, this);
    if (r < 0)
    {
        LOG_ERROR("[BLUEZ] add adv vtable failed: %d", r);
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
        LOG_ERROR("[BLUEZ] RegisterAdvertisement submit failed: %s", strerror(-r));
        return false;
    }
    // Start sd-bus event loop thread (drain and wait)
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

    return true;  // Return true if all above succeeded
#endif
}

void BluezTransport::stop_peripheral()
{
#if BITCHAT_HAVE_SDBUS
    if (impl_->bus)
    {
        sd_bus_message *rep = nullptr;
        sd_bus_error    err = SD_BUS_ERROR_NULL;
        (void)sd_bus_call_method(impl_->bus, "org.bluez", impl_->adapter_path.c_str(),
                                 "org.bluez.LEAdvertisingManager1", "UnregisterAdvertisement",
                                 &err, &rep, "o", impl_->adv_path.c_str());
        sd_bus_error_free(&err);
        if (rep)
            sd_bus_message_unref(rep);
    }
    if (impl_->loop.joinable())
        impl_->loop.join();

    if (impl_->reg_slot)
    {
        sd_bus_slot_unref(impl_->reg_slot);
        impl_->reg_slot = nullptr;
    }
    if (impl_->rx_slot)
    {
        sd_bus_slot_unref(impl_->rx_slot);
        impl_->rx_slot = nullptr;
    }
    if (impl_->tx_slot)
    {
        sd_bus_slot_unref(impl_->tx_slot);
        impl_->tx_slot = nullptr;
    }
    if (impl_->svc_slot)
    {
        sd_bus_slot_unref(impl_->svc_slot);
        impl_->svc_slot = nullptr;
    }
    if (impl_->app_slot)
    {
        sd_bus_slot_unref(impl_->app_slot);
        impl_->app_slot = nullptr;
    }
    if (impl_->adv_call_slot)
    {
        sd_bus_slot_unref(impl_->adv_call_slot);
        impl_->adv_call_slot = nullptr;
    }
    if (impl_->adv_obj_slot)
    {
        sd_bus_slot_unref(impl_->adv_obj_slot);
        impl_->adv_obj_slot = nullptr;
    }

    if (impl_->bus)
    {
        sd_bus_flush(impl_->bus);
        sd_bus_unref(impl_->bus);
        impl_->bus = nullptr;
    }

#endif
}

}  // namespace transport
