#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_mgr_init(void);
bool      wifi_mgr_is_connected(void);
int       wifi_mgr_get_rssi(void);
void      wifi_mgr_get_ip_str(char *buf, size_t len);
void      wifi_mgr_set_low_latency(bool enable);

#ifdef __cplusplus
}
#endif
