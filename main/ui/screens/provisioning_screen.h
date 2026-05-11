#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void provisioning_screen_show_qrcode(const char *payload);
void provisioning_screen_show_status(const char *title, const char *message);

#ifdef __cplusplus
}
#endif
