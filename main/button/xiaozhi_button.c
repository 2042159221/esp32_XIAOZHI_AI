#include "xiaozhi_button.h"

#include "button_adc.h"
#include "esp_check.h"

// ESP-IDF 日志标签。这个文件里所有 ESP_RETURN_ON_ERROR / ESP_RETURN_ON_FALSE
// 打印错误时都会带上 xiaozhi_button，方便从串口日志里定位是哪一层出错。
static const char *TAG = "xiaozhi_button";

// 给当前项目中的 ADC 按键编号取名字。
//
// 注意：这里的编号要和 button_adc_config_t.button_index 保持一致。
// XIAOZHI_BUTTON_COUNT 不是按键数量，而是 adc_buttons[] 数组长度。
// 因为本例直接用 button_index 作为数组下标，所以最大编号是 3 时，数组长度至少要是 4。
enum {
    XIAOZHI_BUTTON_2 = 2,
    XIAOZHI_BUTTON_3 = 3,
    XIAOZHI_BUTTON_COUNT = 4,
};

// 保存每个按键创建成功后的 handle。
//
// button_handle_t 可以理解成“按键对象句柄”。后面注册回调时，必须把对应按键的
// handle 传给 iot_button_register_cb()，按钮组件才知道给哪个按键注册事件。
static button_handle_t adc_buttons[XIAOZHI_BUTTON_COUNT];

// ADC 多按键配置表。
//
// 这是一种表驱动写法：所有硬件相关参数集中放在一个数组里，初始化函数只负责遍历
// 这个表并创建按键。以后新增按键时，优先改这里，而不是复制多段初始化代码。
//
// 本例两个按键共用 ADC_UNIT_1 的通道 7，不同按键通过不同电压区间区分：
// - Button 2: 0 ~ 400 mV
// - Button 3: 1200 ~ 1800 mV
static const button_adc_config_t adc_button_configs[] = {
    {
        // 使用 ADC1。ESP32-S3 上 ADC1 通常更适合做普通采样；ADC2 在一些场景下会和 Wi-Fi 等外设有资源关系。
        .unit_id = ADC_UNIT_1,
        // ADC 通道号。对应哪个 GPIO 要看 ESP32-S3 芯片/开发板引脚映射。
        .adc_channel = 7,
        // 按键编号。按钮组件用它区分同一 ADC 通道上的不同按键。
        .button_index = XIAOZHI_BUTTON_2,
        // 该按键按下时的电压判断区间，单位是 mV。
        .min = 0,
        .max = 400,
    },
    {
        .unit_id = ADC_UNIT_1,
        .adc_channel = 7,
        .button_index = XIAOZHI_BUTTON_3,
        .min = 1200,
        .max = 1800,
    },
};

esp_err_t xiaozhi_button_init(void)
{
    // button_config_t 是通用按键配置，例如消抖时间、长按时间等。
    // 这里全部使用组件默认值，所以置零即可。
    const button_config_t btn_cfg = {0};

    // 遍历 ADC 按键配置表，为每一项创建一个按钮设备。
    // sizeof(adc_button_configs) / sizeof(adc_button_configs[0]) 是 C 语言里计算静态数组元素数量的常见写法。
    for (size_t i = 0; i < sizeof(adc_button_configs) / sizeof(adc_button_configs[0]); i++) {
        // 取出当前这一项配置。用指针可以避免复制整个结构体，也方便传给 ESP-IDF API。
        const button_adc_config_t *adc_config = &adc_button_configs[i];

        // 根据 ADC 配置创建按键对象。
        //
        // 参数说明：
        // 1. &btn_cfg: 通用按键配置，本例使用默认值。
        // 2. adc_config: ADC 通道、电压区间、按键编号等硬件配置。
        // 3. &adc_buttons[adc_config->button_index]: 输出参数，创建成功后保存 handle。
        //
        // ESP_RETURN_ON_ERROR 会检查返回值；如果创建失败，会打印日志并直接 return 错误码。
        ESP_RETURN_ON_ERROR(
            iot_button_new_adc_device(&btn_cfg, adc_config, &adc_buttons[adc_config->button_index]),
            TAG,
            "create adc button %d failed",
            adc_config->button_index);
    }

    return ESP_OK;
}

esp_err_t xiaozhi_button_register_cb(int button_index, button_event_t event, button_cb_t cb, void *usr_data)
{
    // 先检查按键编号是否合法，避免数组越界。
    // 本例有效编号是 2 和 3；编号 0、1、4... 都不应该被使用。
    ESP_RETURN_ON_FALSE(button_index > 0 && button_index < XIAOZHI_BUTTON_COUNT, ESP_ERR_INVALID_ARG, TAG, "invalid button index %d", button_index);

    // 再检查该按键是否已经初始化。
    // 如果 app_main() 忘记先调用 xiaozhi_button_init()，这里会返回 ESP_ERR_INVALID_STATE。
    ESP_RETURN_ON_FALSE(adc_buttons[button_index] != NULL, ESP_ERR_INVALID_STATE, TAG, "button %d is not initialized", button_index);

    // 真正注册回调的地方。
    //
    // 参数说明：
    // 1. adc_buttons[button_index]: 要注册事件的按键对象。
    // 2. event: 事件类型，例如 BUTTON_SINGLE_CLICK、BUTTON_DOUBLE_CLICK、BUTTON_LONG_PRESS_START。
    // 3. NULL: 事件参数。本例没有多击次数、长按时间等额外参数，所以传 NULL。
    // 4. cb: 事件触发时调用的回调函数。
    // 5. usr_data: 用户自定义数据，会原样传给回调函数，常用于传按键编号或业务对象指针。
    return iot_button_register_cb(adc_buttons[button_index], event, NULL, cb, usr_data);
}

esp_err_t xiaozhi_button2_register_cb(button_event_t event, button_cb_t cb, void *usr_data)
{
    // 兼容截图里的教学写法：给 button 2 注册事件。
    // 内部仍然复用通用函数，避免重复检查和重复调用 iot_button_register_cb()。
    return xiaozhi_button_register_cb(XIAOZHI_BUTTON_2, event, cb, usr_data);
}

esp_err_t xiaozhi_button3_register_cb(button_event_t event, button_cb_t cb, void *usr_data)
{
    // 兼容截图里的教学写法：给 button 3 注册事件。
    return xiaozhi_button_register_cb(XIAOZHI_BUTTON_3, event, cb, usr_data);
}
