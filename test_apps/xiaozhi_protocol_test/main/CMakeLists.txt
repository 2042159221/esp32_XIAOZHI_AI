set(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")

idf_component_register(
    SRCS
        "test_xiaozhi_protocol.c"
        "${PROJECT_ROOT}/main/xiaozhi/xiaozhi_protocol.c"
        "${PROJECT_ROOT}/main/xiaozhi/xiaozhi_device.c"
    INCLUDE_DIRS
        "${PROJECT_ROOT}/main/xiaozhi"
    REQUIRES
        unity
        json
        esp_wifi
        esp_netif
        esp_app_format
        nvs_flash)
