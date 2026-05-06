# ESP32-S3 ADC Button Tutorial

这个工程是一个 ESP-IDF 入门练习项目，当前重点是复刻并理解 `espressif/button` 组件的 ADC 多按键功能。

本例在同一个 ADC 通道上识别两个按键：

| 按键 | ADC 单元 | ADC 通道 | 电压范围 |
| ---- | -------- | -------- | -------- |
| Button 2 | `ADC_UNIT_1` | `7` | `0 ~ 400 mV` |
| Button 3 | `ADC_UNIT_1` | `7` | `1200 ~ 1800 mV` |

按下实体按键后，串口日志会打印类似内容：

```text
I (...) app_main: adc button 3 BUTTON_SINGLE_CLICK
I (...) app_main: adc button 2 BUTTON_SINGLE_CLICK
```

## Project Layout

```text
.
├── CMakeLists.txt
├── README.md
├── sdkconfig
├── main
│   ├── CMakeLists.txt
│   ├── idf_component.yml
│   ├── main.c
│   └── button
│       ├── xiaozhi_button.c
│       └── xiaozhi_button.h
└── managed_components
    └── espressif__button
```

关键文件说明：

- `main/main.c`: 应用入口，负责初始化按键并注册事件回调。
- `main/button/xiaozhi_button.c`: 本项目自己的按键封装层，负责创建 ADC 按键设备。
- `main/button/xiaozhi_button.h`: 对外暴露按键初始化和事件注册接口。
- `main/idf_component.yml`: 声明 `espressif/button` 组件依赖。
- `main/CMakeLists.txt`: 告诉 ESP-IDF 哪些 `.c` 文件需要参与编译。
- `.vscode/settings.json`: 保存本机 ESP-IDF 路径、Python 环境、目标芯片和串口配置。

## ESP-IDF Development Flow

ESP-IDF 项目通常按下面的流程开发：

1. 配置开发环境

   VS Code 中已经配置了本项目需要的关键变量：

   ```jsonc
   "idf.espIdfPathWin": "E:/Espressif/frameworks/esp-idf-v5.3.1/",
   "idf.portWin": "COM15",
   "idf.customExtraVars": {
     "IDF_TARGET": "esp32s3",
     "IDF_PYTHON_ENV_PATH": "E:\\Espressif\\python_env\\idf5.3_py3.11_env"
   }
   ```

2. 声明组件依赖

   本例使用官方按钮组件，在 `main/idf_component.yml` 中声明：

   ```yaml
   dependencies:
     espressif/button: ^4.1.6
   ```

   第一次构建时，ESP-IDF Component Manager 会把组件下载到 `managed_components/`。

3. 编写业务代码

   应用入口是 `app_main()`。ESP-IDF 启动 FreeRTOS 和系统组件后，会调用这个函数。

4. 配置 CMake

   新增源文件后，要同步加入 `main/CMakeLists.txt`：

   ```cmake
   idf_component_register(SRCS "main.c" "button/xiaozhi_button.c"
                          INCLUDE_DIRS "." "button")
   ```

5. 构建工程

   推荐在 VS Code 中使用 ESP-IDF 或 CMake Tools 构建。命令行等价流程是：

   ```bat
   cd /D d:\file_store\embedderShit\04_xiaozhi\pro\xiaozhi_sample\ai-xiaozhi-sample
   call E:\Espressif\frameworks\esp-idf-v5.3.1\export.bat
   idf.py build
   ```

6. 烧录固件

   本项目串口配置为 `COM15`：

   ```bat
   idf.py -p COM15 flash
   ```

7. 查看串口日志

   ```bat
   idf.py -p COM15 monitor
   ```

   退出监视器使用 `Ctrl+]`。

## Code Walkthrough

### 1. 应用入口

`main/main.c` 做三件事：

```c
ESP_ERROR_CHECK(xiaozhi_button_init());
ESP_ERROR_CHECK(xiaozhi_button2_register_cb(BUTTON_SINGLE_CLICK, adc_button_cb, (void *)2));
ESP_ERROR_CHECK(xiaozhi_button3_register_cb(BUTTON_SINGLE_CLICK, adc_button_cb, (void *)3));
```

- `xiaozhi_button_init()` 创建 ADC 按键设备。
- `xiaozhi_button2_register_cb(...)` 给按键 2 注册事件回调。
- `xiaozhi_button3_register_cb(...)` 给按键 3 注册事件回调。
- `ESP_ERROR_CHECK(...)` 用来检查返回值；如果初始化失败，会打印错误并终止程序。

回调函数里通过 `usr_data` 区分按键编号，通过 `iot_button_get_event(...)` 读取当前事件：

```c
static void adc_button_cb(void *button_handle, void *usr_data)
{
    int button_index = (int)usr_data;
    button_event_t event = iot_button_get_event(button_handle);

    ESP_LOGI(TAG, "adc button %d %s", button_index, iot_button_get_event_str(event));
}
```

### 2. ADC 按键配置

`main/button/xiaozhi_button.c` 把按键配置集中在数组里：

```c
static const button_adc_config_t adc_button_configs[] = {
    {
        .unit_id = ADC_UNIT_1,
        .adc_channel = 7,
        .button_index = 2,
        .min = 0,
        .max = 400,
    },
    {
        .unit_id = ADC_UNIT_1,
        .adc_channel = 7,
        .button_index = 3,
        .min = 1200,
        .max = 1800,
    },
};
```

这些字段的含义是：

- `unit_id`: 使用哪个 ADC 单元，本例是 `ADC_UNIT_1`。
- `adc_channel`: 使用哪个 ADC 通道，本例是通道 `7`。
- `button_index`: 按键编号，由按钮组件用来区分同一 ADC 通道上的不同按键。
- `min` / `max`: 该按键按下时对应的 ADC 电压区间，单位是 mV。

### 3. 为什么一个 ADC 通道能识别多个按键

多个实体按键可以通过不同电阻接到同一个 ADC 引脚。不同按键按下时，ADC 引脚上的电压不同。

按钮组件会周期性读取 ADC 电压：

```text
ADC 采样值 -> 转换成电压 mV -> 匹配 min/max 区间 -> 判断是哪一个按键 -> 产生按键事件
```

例如本例中：

- 读到 `0 ~ 400 mV`，认为是按键 2。
- 读到 `1200 ~ 1800 mV`，认为是按键 3。

### 4. 事件注册流程

本项目封装了一个通用注册函数：

```c
esp_err_t xiaozhi_button_register_cb(int button_index, button_event_t event, button_cb_t cb, void *usr_data)
```

它内部检查按键是否初始化，然后调用官方组件 API：

```c
iot_button_register_cb(adc_buttons[button_index], event, NULL, cb, usr_data);
```

`iot_button_register_cb` 的五个参数分别是：

| 参数 | 说明 |
| ---- | ---- |
| `btn_handle` | 按键句柄 |
| `event` | 要监听的事件，例如 `BUTTON_SINGLE_CLICK` |
| `event_args` | 某些特殊事件的参数，本例不需要，传 `NULL` |
| `cb` | 事件触发时调用的函数 |
| `usr_data` | 用户自定义数据，本例传按键编号 |

## Common Button Events

`espressif/button` 支持多种事件：

| 事件 | 含义 |
| ---- | ---- |
| `BUTTON_PRESS_DOWN` | 按下 |
| `BUTTON_PRESS_UP` | 松开 |
| `BUTTON_SINGLE_CLICK` | 单击 |
| `BUTTON_DOUBLE_CLICK` | 双击 |
| `BUTTON_LONG_PRESS_START` | 长按开始 |
| `BUTTON_LONG_PRESS_HOLD` | 长按保持 |
| `BUTTON_LONG_PRESS_UP` | 长按后松开 |

要增加事件，只需要继续注册同一个回调或新的回调：

```c
ESP_ERROR_CHECK(xiaozhi_button2_register_cb(BUTTON_DOUBLE_CLICK, adc_button_cb, (void *)2));
ESP_ERROR_CHECK(xiaozhi_button2_register_cb(BUTTON_LONG_PRESS_START, adc_button_cb, (void *)2));
```

## Debug Checklist

如果按了按键没有日志，优先检查下面几点：

1. 串口是否正确：本项目是 `COM15`。
2. 固件是否已经重新烧录。
3. 串口监视器波特率是否是 `115200`。
4. ADC 通道是否和硬件接线一致。
5. 按键按下时电压是否落在配置的 `min/max` 区间。
6. 回调注册时是否传入了正确事件，例如 `BUTTON_SINGLE_CLICK`。
7. `main/CMakeLists.txt` 是否包含新增 `.c` 文件。

## How To Extend

新增一个 ADC 按键时，通常只需要：

1. 在 `adc_button_configs` 中增加一项配置。
2. 确认 `XIAOZHI_BUTTON_COUNT` 大于新的 `button_index`。
3. 在 `app_main()` 中调用 `xiaozhi_button_register_cb(...)` 注册事件。

例如新增按键 4：

```c
{
    .unit_id = ADC_UNIT_1,
    .adc_channel = 7,
    .button_index = 4,
    .min = 2000,
    .max = 2600,
},
```

同时把枚举里的 `XIAOZHI_BUTTON_COUNT` 改成至少 `5`。
