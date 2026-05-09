#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 设备配网状态枚举
 *
 * 该枚举表示配网流程中的关键状态，用于在应用层跟踪当前配网阶段。
 */
typedef enum {
    /** 设备尚未开始配网 */
    APP_PROVISIONING_STATE_UNPROVISIONED = 0,
    /** 设备正在执行配网流程 */
    APP_PROVISIONING_STATE_PROVISIONING,
    /** 已接收到配网凭证（如 SSID/密码） */
    APP_PROVISIONING_STATE_CRED_RECEIVED,
    /** 正在尝试连接到目标 Wi-Fi 网络 */
    APP_PROVISIONING_STATE_CONNECTING,
    /** 已成功连接到 Wi-Fi 网络 */
    APP_PROVISIONING_STATE_CONNECTED,
    /** 配网或连接失败 */
    APP_PROVISIONING_STATE_FAILED,
    /** 配网完成后，应用业务已开始运行 */
    APP_PROVISIONING_STATE_BUSINESS_STARTED,
} app_provisioning_state_t;

/**
 * @brief 当配网业务开始时的回调函数类型
 *
 * @param user_ctx 用户自定义上下文指针，可用于在回调中传递额外信息。
 */
typedef void (*app_provisioning_business_start_cb_t)(void *user_ctx);

/**
 * @brief 配网状态变化回调函数类型
 *
 * @param state 当前配网状态
 * @param user_ctx 用户自定义上下文指针，可用于在回调中传递额外信息。
 */
typedef void (*app_provisioning_state_cb_t)(app_provisioning_state_t state, void *user_ctx);

/**
 * @brief app_provisioning_manager 的配置结构体
 *
 * 该结构体用于传入配网管理器的回调函数和用户上下文信息。
 */
typedef struct {
    /** 业务启动时触发的回调 */
    app_provisioning_business_start_cb_t business_start_cb;
    /** 配网状态变化时触发的回调 */
    app_provisioning_state_cb_t state_cb;
    /** 用户自定义上下文指针，会在回调中透传 */
    void *user_ctx;
} app_provisioning_manager_config_t;

/**
 * @brief 启动配网管理器
 *
 * @param config 指向配网管理器配置结构体的指针，包含回调和用户上下文
 * @return esp_err_t ESP_OK 表示启动成功，其他值表示失败原因
 */
esp_err_t app_provisioning_manager_start(const app_provisioning_manager_config_t *config);

/**
 * @brief 重置配网管理器并重新启动配网流程
 *
 * 该接口通常用于配网失败后的重试场景，清空当前状态并重新进入配网阶段。
 *
 * @return esp_err_t ESP_OK 表示操作成功，其他值表示失败原因
 */
esp_err_t app_provisioning_manager_reset_and_restart(void);

/**
 * @brief 获取当前配网状态
 *
 * @return app_provisioning_state_t 当前配网状态值
 */
app_provisioning_state_t app_provisioning_manager_get_state(void);

#ifdef __cplusplus
}
#endif