// include/transport/bluez_helper_central.hpp
#pragma once

#if BITCHAT_HAVE_SDBUS
#include <systemd/sd-bus.h>

namespace transport
{

// Central-side DBus callbacks
int bluez_on_iface_added(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
int bluez_on_iface_removed(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
int bluez_on_props_changed(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
int bluez_on_connect_reply(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);

}  // namespace transport
#endif  // BITCHAT_HAVE_SDBUS
