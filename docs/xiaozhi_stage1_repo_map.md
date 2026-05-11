# Xiaozhi Stage1 Repo Map

## Current Layout

- `main/app/`
  - `app_main.c`: boot entry, calls platform init then controller start.
  - `app_controller.c`: current business dispatcher after provisioning/WiFi success.
  - `app_input_controller.c`: button/reset input handling.
- `main/platform/`
  - `esp_platform_init.c`: NVS, `esp_netif`, default event loop init.
- `main/services/network/`
  - `wifi_sta_service.c`: STA init/connect/disconnect and IP event handling.
- `main/services/provisioning/`
  - `provisioning_service.c`: provisioning flow and success callback chain.
  - `provisioning_qr_payload.c`: QR payload builder.
- `main/ui/display/`
  - `display_service.c`: current LVGL screen init and QR/message rendering.
- `main/ui/screens/`
  - `provisioning_screen.c`: UI wrapper for provisioning states.
- `main/bsp/lcd/`
  - `bsp_lcd.c`: LCD panel bring-up.
- `main/bsp/button/`, `main/bsp/led/`
  - board peripherals.
- `main/idf_component.yml`
  - current managed dependencies: button, qrcode, esp_lvgl_port, led_strip, lvgl.

## Key Hooks

- WiFi got IP / connected event:
  - `main/services/network/wifi_sta_service.c`
  - `IP_EVENT_STA_GOT_IP` in `wifi_event_handler()`
- Provisioning success callback:
  - `main/services/provisioning/provisioning_service.c`
  - `WIFI_PROV_CRED_SUCCESS` -> `start_business()`
- Business entry point:
  - `main/app/app_controller.c`
  - `business_start_cb()`

## Display / LVGL

- `display_service_init()` creates the current LVGL port and base screen.
- `display_service_show_qrcode()` renders the provisioning QR code.
- `display_service_show_message()` renders title + message.
- LVGL is already protected by `lvgl_port_lock()` / `lvgl_port_unlock()`.

## Existing Dependencies

- `cJSON`: already present through ESP-IDF `json` component.
- `esp_lvgl_port`: already present.
- `esp_lcd`: already present through BSP LCD layer.
- `esp_http_client`: available in IDF and linked in current build artifacts, but not yet in project sources.
- `esp_app_desc.h`: available in IDF v5.3.1.

## Risks

- Current UI is generic and English-centric; new stage1 UI should reuse the existing LVGL port but replace screen content.
- No dedicated Xiaozhi shared handle, device identity helper, OTA client, or websocket placeholder exists yet.
- Existing `display_service` is a reusable but minimal base; avoid duplicating screen systems unless necessary.

## Recommended Modify Set

- `main/xiaozhi/xiaozhi_handle.*`
- `main/xiaozhi/xiaozhi_device.*`
- `main/xiaozhi/xiaozhi_ota.*`
- `main/xiaozhi/xiaozhi_ws.*`
- `main/ui/display/xiaozhi_ui.*`
- `main/app/xiaozhi_stage1.*` or equivalent integration glue
- `main/app/app_controller.c`
- `main/services/provisioning/provisioning_service.c` if hook placement needs adjustment
- `main/idf_component.yml`
- `main/CMakeLists.txt`
- `main/Kconfig.projbuild`

