// include/transport/bluez_helper_peripheral.hpp
#pragma once

#if BITCHAT_HAVE_SDBUS
#include <systemd/sd-bus.h>

extern const sd_bus_vtable gatt_service_vtable[];
extern const sd_bus_vtable gatt_tx_vtable[];
extern const sd_bus_vtable gatt_rx_vtable[];
extern const sd_bus_vtable adv_vtable[];

// Callbacks for peripheral role
extern int on_reg_app_reply(sd_bus_message *m, void *userdata, sd_bus_error *);
extern int tx_StartNotify(sd_bus_message *m, void *userdata, sd_bus_error *);
extern int tx_StopNotify(sd_bus_message *m, void *userdata, sd_bus_error *);
extern int rx_WriteValue(sd_bus_message *m, void *userdata, sd_bus_error *);
extern int adv_Release(sd_bus_message *m, void *userdata, sd_bus_error *);
extern int on_reg_adv_reply(sd_bus_message *m, void *userdata, sd_bus_error *);

extern int svc_prop_UUID(sd_bus *,
                         const char *,
                         const char *,
                         const char *,
                         sd_bus_message *reply,
                         void           *userdata,
                         sd_bus_error *);
extern int svc_prop_Primary(sd_bus *,
                            const char *,
                            const char *,
                            const char *,
                            sd_bus_message *reply,
                            void           *userdata,
                            sd_bus_error *);
extern int svc_prop_Includes(sd_bus *,
                             const char *,
                             const char *,
                             const char *,
                             sd_bus_message *reply,
                             void           *userdata,
                             sd_bus_error *);
extern int chr_prop_UUID(sd_bus *,
                         const char *path,
                         const char *,
                         const char *,
                         sd_bus_message *reply,
                         void           *userdata,
                         sd_bus_error *);
extern int chr_prop_Service(sd_bus *,
                            const char *,
                            const char *,
                            const char *,
                            sd_bus_message *reply,
                            void           *userdata,
                            sd_bus_error *);
extern int chr_prop_Flags(sd_bus *,
                          const char *path,
                          const char *,
                          const char *,
                          sd_bus_message *reply,
                          void           *userdata,
                          sd_bus_error *);
extern int chr_prop_Notifying(sd_bus *,
                              const char *path,
                              const char *,
                              const char *,
                              sd_bus_message *reply,
                              void           *userdata,
                              sd_bus_error *);
extern int adv_prop_Type(sd_bus *,
                         const char *,
                         const char *,
                         const char *,
                         sd_bus_message *reply,
                         void *,
                         sd_bus_error *);
extern int adv_prop_ServiceUUIDs(sd_bus *,
                                 const char *,
                                 const char *,
                                 const char *,
                                 sd_bus_message *reply,
                                 void           *userdata,
                                 sd_bus_error *);
extern int adv_prop_LocalName(sd_bus *,
                              const char *,
                              const char *,
                              const char *,
                              sd_bus_message *reply,
                              void *,
                              sd_bus_error *);
extern int adv_prop_IncludeTxPower(sd_bus *,
                                   const char *,
                                   const char *,
                                   const char *,
                                   sd_bus_message *reply,
                                   void *,
                                   sd_bus_error *);

#endif  // BITCHAT_HAVE_SDBUS
