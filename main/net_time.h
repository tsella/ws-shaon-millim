#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

extern volatile bool g_wifi_connected;
extern volatile bool g_wifi_sleeping;
extern volatile int  g_wifi_rssi;
extern char          g_wifi_ip[32];
extern volatile bool g_ntp_synced;

void net_time_init(void);
void net_time_wake(void);
void net_time_reconnect(void);
void net_time_start_scan(void);
void net_time_poll_rssi(void);

#ifdef __cplusplus
}
#endif
