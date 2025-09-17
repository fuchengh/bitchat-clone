#include "transport/bluez_transport.hpp"
#include "util/log.hpp"

#include <cstring>
#include <mutex>

#if BITCHAT_HAVE_SDBUS
#include <poll.h>
#include <systemd/sd-bus.h>
#include <unistd.h>
#endif

#if BITCHAT_HAVE_SDBUS
namespace
{
static int emit_props_changed_value(sd_bus        *bus,
                                    const char    *path,
                                    const uint8_t *bytes,
                                    size_t         len)
{
    if (!bus || !path)
        return -EINVAL;
    sd_bus_message *sig = nullptr;
    int r = sd_bus_message_new_signal(bus, &sig, path, "org.freedesktop.DBus.Properties",
                                      "PropertiesChanged");
    // clang-format off
    if (r < 0) goto fail;
    // interface name
    r = sd_bus_message_append(sig, "s", "org.bluez.GattCharacteristic1"); if (r < 0) goto fail;
    // a{sv}
    r = sd_bus_message_open_container(sig, SD_BUS_TYPE_ARRAY, "{sv}");    if (r < 0) goto fail;
    // dict entry: "Value" -> variant "ay"
    r = sd_bus_message_open_container(sig, SD_BUS_TYPE_DICT_ENTRY, "sv"); if (r < 0) goto fail;
    r = sd_bus_message_append(sig, "s", "Value");                         if (r < 0) goto fail;
    r = sd_bus_message_open_container(sig, SD_BUS_TYPE_VARIANT, "ay");    if (r < 0) goto fail;
    r = sd_bus_message_append_array(sig, 'y', bytes, len);                if (r < 0) goto fail;
    r = sd_bus_message_close_container(sig); /* variant */                if (r < 0) goto fail;
    r = sd_bus_message_close_container(sig); /* dict entry */             if (r < 0) goto fail;
    r = sd_bus_message_close_container(sig); /* a{sv} */                  if (r < 0) goto fail;
    // invalidated properties: empty 'as'
    r = sd_bus_message_open_container(sig, SD_BUS_TYPE_ARRAY, "s");       if (r < 0) goto fail;
    r = sd_bus_message_close_container(sig);                              if (r < 0) goto fail;
    // send
    r = sd_bus_send(bus, sig, nullptr);
  fail:
    // clang-format on
    if (sig)
        sd_bus_message_unref(sig);
    return r;
}
}  // namespace
#endif

namespace transport
{

bool BluezTransport::peripheral_can_notify()
{
#if BITCHAT_HAVE_SDBUS
    if (!impl_ || bus())
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

bool BluezTransport::peripheral_send_locked(const Frame &f)
{
#if BITCHAT_HAVE_SDBUS
    std::lock_guard<std::mutex> lk(bus_mu());
    const int r = emit_props_changed_value(bus(), tx_path().c_str(), f.data(), f.size());
    if (r < 0)
    {
        LOG_WARN("[BLUEZ][peripheral] notify send failed: %s", strerror(-r));
        return false;
    }
    LOG_DEBUG("[BLUEZ][peripheral] notify len=%zu sent", (size_t)f.size());
    return true;
#else
    (void)f;
    return false;
#endif
}

bool BluezTransport::peripheral_notify_value(const Frame &f)
{
#if BITCHAT_HAVE_SDBUS
    if (!peripheral_can_notify())
        return false;
    return peripheral_send_locked(f);
#else
    (void)f;
    return false;
#endif
}

bool BluezTransport::send_peripheral_impl(const Frame &f)
{
    return peripheral_notify_value(f);
}

}  // namespace transport
