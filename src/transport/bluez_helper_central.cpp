// include/transport/bluez_helper_central.hpp
#include "transport/bluez_transport.hpp"
#include "transport/bluez_helper_central.hpp"
#include "transport/bluez_dbus_util.hpp"

#include <string>
#include <cstring>

#if BITCHAT_HAVE_SDBUS
#include <systemd/sd-bus.h>

namespace transport
{

int bluez_on_iface_added(sd_bus_message *m, void *userdata, sd_bus_error * /*ret*/)
{
    auto *self = static_cast<transport::BluezTransport *>(userdata);

    const char *obj = nullptr;
    int         r   = sd_bus_message_read(m, "o", &obj);
    if (r < 0 || !obj)
        return r < 0 ? r : -EINVAL;

    const std::string obj_path(obj);
    const std::string prefix = "/org/bluez/" + self->config().adapter + "/dev_";
    if (obj_path.rfind(prefix, 0) != 0)
        return 0;

    bool        svc_hit = false;
    std::string addr;
    int16_t     rssi      = 0;
    bool        have_rssi = false;

    r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "{sa{sv}}");
    if (r < 0)
        return r;
    while ((r = sd_bus_message_enter_container(m, SD_BUS_TYPE_DICT_ENTRY, "sa{sv}")) > 0)
    {
        const char *iface = nullptr;
        if ((r = sd_bus_message_read(m, "s", &iface)) < 0)
            return r;

        if (iface && strcmp(iface, "org.bluez.Device1") == 0)
        {
            if ((r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "{sv}")) < 0)
                return r;

            while ((r = sd_bus_message_enter_container(m, SD_BUS_TYPE_DICT_ENTRY, "sv")) > 0)
            {
                const char *key = nullptr;
                if ((r = sd_bus_message_read(m, "s", &key)) < 0)
                    return r;

                if (key && strcmp(key, "UUIDs") == 0)
                {
                    bool hit = false;
                    if ((r = var_as_has_uuid(m, self->config().svc_uuid, hit)) < 0)
                        return r;
                    svc_hit |= hit;
                }
                else if (key && strcmp(key, "Address") == 0)
                {
                    if ((r = read_var_s(m, addr)) < 0)
                        return r;
                }
                else if (key && strcmp(key, "RSSI") == 0)
                {
                    if ((r = read_var_i16(m, rssi)) < 0)
                        return r;
                    have_rssi = true;
                }
                else
                {
                    if ((r = sd_bus_message_skip(m, "v")) < 0)
                        return r;
                }

                if ((r = sd_bus_message_exit_container(m)) < 0)
                    return r;

                if (svc_hit &&
                    (!self->config().peer_addr || mac_eq(addr, *self->config().peer_addr)))
                {
                    while (sd_bus_message_at_end(m, 0) == 0)
                    {
                        int rr = sd_bus_message_skip(m, "{sv}");
                        if (rr < 0)
                            return rr;
                    }
                    break;
                }
            }
            if ((r = sd_bus_message_exit_container(m)) < 0)
                return r;
        }
        else
        {
            if ((r = sd_bus_message_skip(m, "a{sv}")) < 0)
                return r;
        }

        if ((r = sd_bus_message_exit_container(m)) < 0)
            return r;
        if ((r = sd_bus_message_exit_container(m)) < 0)
            return r;
    }

    if (r < 0)
        return r;
    if ((r = sd_bus_message_exit_container(m)) < 0)
        return r;

    if (!svc_hit && self->has_uuid_discovery_filter())
        svc_hit = true;

    bool has_peer = (self->config().peer_addr && !self->config().peer_addr->empty());
    bool peer_ok  = (has_peer && !addr.empty() && mac_eq(addr, *self->config().peer_addr));

    // REMOVE for now (can be enabled for debugging)
    // static const bool strict_peer = !!getenv("BITCHAT_PEER_STRICT") &&
    //                                 std::string(getenv("BITCHAT_PEER_STRICT")) == "1";
    // LOG_DEBUG("[BLUEZ][central] iface-added decision: svc_hit=%d has_peer=%d addr=%s peer=%s "
    //           "match=%d strict=%d",
    //           (int)svc_hit, (int)has_peer, addr.empty() ? "?" : addr.c_str(),
    //           has_peer ? self->config().peer_addr->c_str() : "-", (int)peer_ok, (int)strict_peer);
    if (has_peer)
    {
        if (peer_ok)
        {
            // OK
        }
        else if (svc_hit)  // && !strict_peer)
        {
            LOG_DEBUG("[BLUEZ][central] peer MAC mismatch but service UUID hit (likely RPA) -> "
                      "accept");
        }
        else
            return 0;
    }
    else
    {
        if (!svc_hit)
            return 0;
    }

    if (self->dev_path().empty())
    {
        self->set_dev_path(obj);
        if (have_rssi)
        {
            LOG_SYSTEM("[BLUEZ][central] found %s addr=%s rssi=%d (svc hit)", obj,
                       addr.empty() ? "?" : addr.c_str(), (int)rssi);
        }
        else
        {
            LOG_SYSTEM("[BLUEZ][central] found %s addr=%s (svc hit)", obj,
                       addr.empty() ? "?" : addr.c_str());
        }
    }
    return 0;
}

int bluez_on_iface_removed(sd_bus_message *m, void *userdata, sd_bus_error * /*ret*/)
{
    auto       *self = static_cast<transport::BluezTransport *>(userdata);
    const char *obj  = nullptr;
    int         r    = sd_bus_message_read(m, "o", &obj);
    if (r < 0 || !obj)
        return r < 0 ? r : -EINVAL;
    r = sd_bus_message_skip(m, "as");
    if (r < 0)
        return r;

    const std::string path(obj);
    if (!self->dev_path().empty() && self->dev_path() == path)
    {
        self->set_connected(false);
        self->set_subscribed(false);
        self->set_dev_path("");
        LOG_SYSTEM("[BLUEZ][central] InterfacesRemoved -> cleared device %s", obj);
    }
    return 0;
}

int bluez_on_props_changed(sd_bus_message *m, void *userdata, sd_bus_error * /*ret_error*/)
{
    auto       *self  = static_cast<transport::BluezTransport *>(userdata);
    const char *iface = nullptr;
    int         r     = sd_bus_message_read(m, "s", &iface);
    if (r < 0)
        return r;

    bool services_resolved_hit = false;
    bool services_resolved_val = false;
    bool connected_hit         = false;
    bool connected_val         = false;
    // Device1.UUIDs support
    bool uuids_hit         = false;
    bool uuids_has_service = false;

    bool        value_hit = false;
    const void *val_buf   = nullptr;
    size_t      val_len   = 0;

    r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "{sv}");
    if (r < 0)
        return r;

    while ((r = sd_bus_message_enter_container(m, SD_BUS_TYPE_DICT_ENTRY, "sv")) > 0)
    {
        const char *key = nullptr;
        r               = sd_bus_message_read(m, "s", &key);
        if (r < 0)
            return r;

        if (key && strcmp(key, "ServicesResolved") == 0 && iface &&
            strcmp(iface, "org.bluez.Device1") == 0)
        {
            r = sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, "b");
            if (r < 0)
                return r;
            int b = 0;
            r     = sd_bus_message_read(m, "b", &b);
            if (r < 0)
                return r;
            services_resolved_hit = true;
            services_resolved_val = (b != 0);
            r                     = sd_bus_message_exit_container(m);
            if (r < 0)
                return r;
        }
        else if (key && strcmp(key, "Connected") == 0 && iface &&
                 strcmp(iface, "org.bluez.Device1") == 0)
        {
            r = sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, "b");
            if (r < 0)
                return r;
            int b = 0;
            r     = sd_bus_message_read(m, "b", &b);
            if (r < 0)
                return r;
            connected_hit = true;
            connected_val = (b != 0);
            r             = sd_bus_message_exit_container(m);
            if (r < 0)
                return r;
        }
        else if (key && strcmp(key, "UUIDs") == 0 && iface &&
                 strcmp(iface, "org.bluez.Device1") == 0)
        {
            // variant(as) – list of service UUIDs
            bool hit = false;
            r        = var_as_has_uuid(m, self->config().svc_uuid, hit);
            if (r < 0)
                return r;
            uuids_hit = true;
            if (hit)
                uuids_has_service = true;
        }
        else if (key && strcmp(key, "Value") == 0 && iface &&
                 strcmp(iface, "org.bluez.GattCharacteristic1") == 0)
        {
            r = sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, "ay");
            if (r < 0)
                return r;
            r = sd_bus_message_read_array(m, 'y', &val_buf, &val_len);
            if (r < 0)
                return r;
            value_hit = true;
            r         = sd_bus_message_exit_container(m);
            if (r < 0)
                return r;
        }
        else
        {
            r = sd_bus_message_skip(m, "v");
            if (r < 0)
                return r;
        }
        r = sd_bus_message_exit_container(m);
        if (r < 0)
            return r;
    }
    if (r < 0)
        return r;
    r = sd_bus_message_exit_container(m);
    if (r < 0)
        return r;

    r = sd_bus_message_skip(m, "as");
    if (r < 0)
        return r;

    const char *path = sd_bus_message_get_path(m);

    // If UUIDs arrived later and match our service, and we don't have a device yet,
    // adopt this device only when no peer MAC filter is set
    if (path && self->dev_path().empty() && uuids_hit && uuids_has_service)
    {
        bool has_peer = (self->config().peer_addr && !self->config().peer_addr->empty());
        if (!has_peer)
        {
            const std::string dev_prefix = std::string("/org/bluez/") + self->config().adapter +
                                           "/dev_";
            if (std::string(path).rfind(dev_prefix, 0) == 0)
            {
                self->set_dev_path(path);
                LOG_DEBUG("[BLUEZ][central] PropertiesChanged(UUIDs) picked device: %s", path);
            }
        }
    }

    if (path && self->dev_path() == path && connected_hit)
    {
        if (connected_val && !self->connected())
        {
            self->set_connected(true);
            LOG_SYSTEM("[BLUEZ][central] Connected property became true (%s)", path);
        }
        else if (!connected_val && self->connected())
        {
            self->set_connected(false);
            self->set_subscribed(false);
            LOG_SYSTEM("[BLUEZ][central] Disconnected (%s)", path);
        }
    }

    if (path && self->dev_path() == path && services_resolved_hit && services_resolved_val)
    {
        self->set_services_resolved(true);
        LOG_SYSTEM("[BLUEZ][central] ServicesResolved=true on %s", path);
    }
    else if (path && self->dev_path() == path && services_resolved_hit && !services_resolved_val)
    {
        self->set_services_resolved(false);
        LOG_SYSTEM("[BLUEZ][central] ServicesResolved=false on %s", path);
    }

    if (value_hit)
    {
        const std::string dev_prefix = self->dev_path() + "/";
        if (path && std::strncmp(path, dev_prefix.c_str(), dev_prefix.size()) == 0)
        {
            LOG_DEBUG("[BLUEZ][central] notify on %s len=%zu", path ? path : "?", val_len);
            if (val_buf && val_len)
            {
                self->deliver_rx_bytes(static_cast<const uint8_t *>(val_buf), val_len);
            }
        }
    }

    return 0;
}

int bluez_on_connect_reply(sd_bus_message *m, void *userdata, sd_bus_error * /*ret*/)
{
    auto *self = static_cast<transport::BluezTransport *>(userdata);

    self->set_connect_inflight(false);

    if (sd_bus_message_is_method_error(m, nullptr))
    {
        const sd_bus_error *e          = sd_bus_message_get_error(m);
        const char         *ename      = (e && e->name) ? e->name : "unknown";
        const char         *emsg       = (e && e->message) ? e->message : "no message";
        uint32_t            backoff_ms = 2000;
        if (strcmp(ename, "org.freedesktop.DBus.Error.NoReply") == 0 ||
            strcmp(ename, "org.bluez.Error.InProgress") == 0 ||
            (strcmp(ename, "org.bluez.Error.Failed") == 0 &&
             (emsg && strstr(emsg, "already in progress"))))
        {
            backoff_ms = 5000;
            LOG_WARN("[BLUEZ][central] Connect in progress/timeouts, backoff %ums: %s: %s",
                     backoff_ms, ename, emsg);
        }
        else
        {
            LOG_ERROR("[BLUEZ][central] Device1.Connect failed, backoff %ums: %s: %s", backoff_ms,
                      ename, emsg);
        }
        self->set_connected(false);
        self->set_subscribed(false);
        // If the device object disappeared, clear dev_path so pump will re-scan
        if (strcmp(ename, "org.freedesktop.DBus.Error.UnknownObject") == 0 ||
            strcmp(ename, "org.freedesktop.DBus.Error.UnknownMethod") == 0)
        {
            self->set_dev_path("");
            LOG_DEBUG("[BLUEZ][central] Cleared device path after UnknownObject/Method");
        }
        // set next_try time
        uint64_t now_ms =
            (uint64_t)std::chrono::duration_cast<
                std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count();
        self->set_next_connect_at_ms(now_ms + backoff_ms);
        return 1;
    }

    self->set_connected(true);
    LOG_SYSTEM("[BLUEZ][central] Device connected: %s", self->dev_path().c_str());
    self->set_services_resolved(false);
    return 1;
}

}  // namespace transport

#endif