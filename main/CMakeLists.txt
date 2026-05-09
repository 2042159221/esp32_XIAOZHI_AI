idf_component_register(SRCS "main.c"
                "bsp_lcd.c"
                "app_display.c"
                "app_ble_prov.c"
                "button/xiaozhi_button.c"
                "wifi_sta/xiaozhi_wifi_sta.c"
                "app_provisioning/app_provisioning_adapter.c"
                "app_provisioning/app_provisioning_manager.c"
                "app_provisioning/app_provisioning_strategies.c"
            INCLUDE_DIRS "." "button" "wifi_sta" "app_provisioning"
            REQUIRES esp_wifi esp_netif esp_event nvs_flash wifi_provisioning esp_lcd lvgl driver
            PRIV_REQUIRES esp_timer)
