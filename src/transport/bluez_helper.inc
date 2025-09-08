// bluez helpers
#include <cctype>
#include <cerrno>
#include <cstring>
#include <memory>
#include <vector>

#include "transport/bluez_transport.hpp"
#include "util/log.hpp"

#if BITCHAT_HAVE_SDBUS
#include <systemd/sd-bus.h>

static int on_reg_app_reply(sd_bus_message *m, void *userdata, sd_bus_error * /*ret_error*/)
{
    auto *self = static_cast<transport::BluezTransport *>(userdata);
    if (sd_bus_message_is_method_error(m, nullptr))
    {
        const sd_bus_error *e = sd_bus_message_get_error(m);
        LOG_ERROR("[BLUEZ] RegisterApplication failed: %s: %s", e && e->name ? e->name : "unknown",
                  e && e->message ? e->message : "no message");
        self->set_reg_ok(false);
    }
    else
    {
        LOG_DEBUG("[BLUEZ] GATT app registered at %s (bus=%s)", self->app_path().c_str(),
                  self->unique_name().c_str());
        self->set_reg_ok(true);
    }
    return 1;
}

// Service props
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
    return sd_bus_message_append(reply, "b", 1);
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

// Char props
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
    int         r;

    if (p == self->tx_path())
    {
        r = sd_bus_message_open_container(reply, 'a', "s");
        if (r < 0)
            return r;
        r = sd_bus_message_append_basic(reply, 's', "notify");
        if (r < 0)
            return r;
        r = sd_bus_message_append_basic(reply, 's', "read");
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
        r = sd_bus_message_append_basic(reply, 's', "write");
        if (r < 0)
            return r;
        r = sd_bus_message_append_basic(reply, 's', "write-without-response");
        if (r < 0)
            return r;
        return sd_bus_message_close_container(reply);
    }
    return -EINVAL;
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

// TX methods
static int tx_StartNotify(sd_bus_message *m, void *userdata, sd_bus_error * /*ret_err*/)
{
    auto *self = static_cast<transport::BluezTransport *>(userdata);
    self->set_tx_notifying(true);
    self->emit_tx_props_changed("Notifying");
    LOG_DEBUG("[BLUEZ] tx.StartNotify");
    return sd_bus_reply_method_return(m, "");
}

static int tx_StopNotify(sd_bus_message *m, void *userdata, sd_bus_error * /*ret_err*/)
{
    auto *self = static_cast<transport::BluezTransport *>(userdata);
    self->set_tx_notifying(false);
    self->emit_tx_props_changed("Notifying");
    LOG_DEBUG("[BLUEZ] tx.StopNotify");
    return sd_bus_reply_method_return(m, "");
}

// RX method
static int rx_WriteValue(sd_bus_message *m, void *userdata, sd_bus_error * /*ret_err*/)
{
    auto       *self = static_cast<transport::BluezTransport *>(userdata);
    const void *buf  = nullptr;
    size_t      len  = 0;

    int r = sd_bus_message_read_array(m, 'y', &buf, &len);
    if (r < 0)
        return r;
    LOG_DEBUG("[BLUEZ] rx.WriteValue len=%zu", len);

    uint16_t    offset = 0;
    const char *type   = nullptr;

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
        else if (key && std::strcmp(key, "type") == 0)
        {
            if ((r = sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, "s")) < 0)
                return r;
            (void)sd_bus_message_read(m, "s", &type);
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

// Advertising
static int adv_Release(sd_bus_message *m, void * /*userdata*/, sd_bus_error * /*ret*/)
{
    LOG_DEBUG("[BLUEZ] adv.Release()");
    return sd_bus_reply_method_return(m, "");
}

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
    return sd_bus_message_append(reply, "b", 0);
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
        LOG_INFO("[BLUEZ] LE advertisement registered successfully");
    }
    return 1;
}

// Utils
static inline bool ieq(std::string a, std::string b)
{
    auto norm = [](std::string s) {
        for (auto &c : s)
            c = (char)std::tolower((unsigned char)c);
        return s;
    };
    return norm(std::move(a)) == norm(std::move(b));
}

static inline bool mac_eq(std::string a, std::string b)
{
    auto norm = [](std::string s) {
        for (auto &c : s)
            c = (char)std::toupper((unsigned char)c);
        return s;
    };
    return norm(std::move(a)) == norm(std::move(b));
}

static inline bool path_mac_eq(const std::string &obj_path, const std::string &mac)
{
    auto pos = obj_path.rfind("/dev_");
    if (pos == std::string::npos)
        return false;
    std::string tail = obj_path.substr(pos + 5);
    for (auto &c : tail)
        if (c == '_')
            c = ':';
    return mac_eq(tail, mac);
}

static int read_var_s(sd_bus_message *m, std::string &out)
{
    int r = sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, "s");
    if (r < 0)
        return r;
    const char *s = nullptr;
    r             = sd_bus_message_read(m, "s", &s);
    if (r >= 0 && s)
        out = s;
    int r2 = sd_bus_message_exit_container(m);
    return r < 0 ? r : r2;
}

static int read_var_i16(sd_bus_message *m, int16_t &out)
{
    int r = sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, "n");
    if (r < 0)
        return r;
    r      = sd_bus_message_read(m, "n", &out);
    int r2 = sd_bus_message_exit_container(m);
    return r < 0 ? r : r2;
}

static int var_as_has_uuid(sd_bus_message *m, const std::string &want_uuid, bool &hit)
{
    hit   = false;
    int r = sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, "as");
    if (r < 0)
        return r;
    r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "s");
    if (r < 0)
        return r;
    while (true)
    {
        const char *u  = nullptr;
        int         rr = sd_bus_message_read_basic(m, 's', &u);
        if (rr <= 0)
            break;
        if (u && ieq(u, want_uuid))
            hit = true;
    }
    int r1 = sd_bus_message_exit_container(m);
    int r2 = sd_bus_message_exit_container(m);
    return (r < 0 || r1 < 0 || r2 < 0) ? -1 : 0;
}

// Central callbacks
static int on_iface_added(sd_bus_message *m, void *userdata, sd_bus_error * /*ret*/)
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

        if ((r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "{sv}")) < 0)
            return r;

        if (iface && strcmp(iface, "org.bluez.Device1") == 0)
        {
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

    bool has_peer = (self->config().peer_addr && !self->config().peer_addr->empty());
    bool peer_ok  = (has_peer && !addr.empty() && mac_eq(addr, *self->config().peer_addr));
    static const bool strict_peer = !!getenv("BITCHAT_PEER_STRICT") &&
                                    std::string(getenv("BITCHAT_PEER_STRICT")) == "1";
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
        else if (svc_hit && !strict_peer)
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
            LOG_DEBUG("[BLUEZ][central] found %s addr=%s rssi=%d (svc hit)", obj,
                      addr.empty() ? "?" : addr.c_str(), (int)rssi);
        }
        else
        {
            LOG_DEBUG("[BLUEZ][central] found %s addr=%s (svc hit)", obj,
                      addr.empty() ? "?" : addr.c_str());
        }
    }
    return 0;
}

static int on_props_changed(sd_bus_message *m, void *userdata, sd_bus_error * /*ret_error*/)
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
            LOG_INFO("[BLUEZ][central] Connected property became true (%s)", path);
        }
        else if (!connected_val && self->connected())
        {
            self->set_connected(false);
            self->set_subscribed(false);
            LOG_INFO("[BLUEZ][central] Disconnected (%s)", path);
        }
    }

    if (path && self->dev_path() == path && services_resolved_hit && services_resolved_val)
        LOG_DEBUG("[BLUEZ][central] ServicesResolved=true on %s", path);

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

static int on_connect_reply(sd_bus_message *m, void *userdata, sd_bus_error * /*ret*/)
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
    LOG_INFO("[BLUEZ][central] Device connected: %s", self->dev_path().c_str());
    return 1;
}

static int on_iface_removed(sd_bus_message *m, void *userdata, sd_bus_error * /*ret*/)
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
        LOG_DEBUG("[BLUEZ][central] InterfacesRemoved -> cleared device %s", obj);
    }
    return 0;
}

#endif