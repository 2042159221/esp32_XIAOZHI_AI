set(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")

idf_component_register(
    SRCS
        "audio_board_test_main.c"
        "${PROJECT_ROOT}/main/platform/esp_platform_init.c"
        "${PROJECT_ROOT}/main/services/network/wifi_sta_service.c"
        "${PROJECT_ROOT}/main/bsp/audio/bsp_audio.c"
        "${PROJECT_ROOT}/main/services/audio/audio_pcm_service.c"
        "${PROJECT_ROOT}/main/services/audio/audio_pcm_diag.c"
        "${PROJECT_ROOT}/main/services/audio/audio_opus_codec.c"
        "${PROJECT_ROOT}/main/services/audio/audio_opus_diag.c"
    INCLUDE_DIRS
        "."
        "${PROJECT_ROOT}/main/board"
        "${PROJECT_ROOT}/main/platform"
        "${PROJECT_ROOT}/main/services/network"
        "${PROJECT_ROOT}/main/bsp/audio"
        "${PROJECT_ROOT}/main/services/audio"
    REQUIRES
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        driver
        esp_websocket_client
        esp_codec_dev
        esp_audio_codec
    PRIV_REQUIRES
        esp_timer)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -finput-charset=UTF-8
    -fexec-charset=UTF-8)
