#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void app_ble_prov_show_qrcode(const char *payload);
void app_ble_prov_show_status(const char *title, const char *message);

#ifdef __cplusplus
}
#endif
