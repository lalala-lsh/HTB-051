# HTB-051 整体代码架构文档

本文档从技术层面描述 HTB-051 智能灯光控制系统固件的整体架构设计，包括目录划分、分层设计、模块职责、数据流与依赖关系。

---

## 1. 项目概述

| 项目 | 说明 |
|------|------|
| **目标芯片** | ESP32 / ESP32-S3 / ESP32-C3 |
| **开发框架** | ESP-IDF 5.3.1 |
| **固件版本** | 1.3.1 |
| **设计思想** | 面向对象式 C 语言：不透明类型、create/destroy 生命周期、getter/setter、回调驱动 |

---

## 2. 目录划分原则与总览

代码目录**按需求域划分**，每个域对应一类业务能力，便于职责清晰与并行开发。

| 需求域 | 对应目录/文件 | 核心职责简述 |
|--------|----------------|---------------|
| **音频需求** | `main/Audio/` | 本地 MP3 播放、音频队列、A2DP 蓝牙接收、本地/蓝牙模式切换 |
| **物联网需求** | `main/Iot/` | MQTT 客户端、协议编解码、参数处理调度、OTA、SNTP |
| **灯光控制需求** | `main/light_control/` | 多路灯光状态机、亮度档位、光疗模式、按键扫描、PWM/触摸硬件抽象 |
| **传感器控制需求** | `main/sensor_control/` | PIR 人体感应、BH1750 恒光控制、MQTT 参数处理 |
| **其余系统** | `main/` 根目录 | 入口、引脚、设备参数、NVS、出厂设置、系统信息、构建配置 |

整体目录树如下：

```
htb-051/
├── main/                          # 主应用程序
│   ├── app_main.c                 # 程序入口，初始化与模块启动顺序
│   ├── board_pins.h               # 硬件引脚定义（GPIO 映射）
│   ├── CMakeLists.txt            # 主程序构建配置
│   ├── device_params.c/h         # 设备参数单例（NVS 持久化 + 只读/可写接口）
│   ├── factory.c/h               # 恢复出厂设置、SN 码初始化
│   ├── settings.c/h              # NVS 封装（OOP 风格）
│   ├── system_info.c/h           # 系统信息管理（SN、MAC 等）
│   ├── idf_component.yml        # 组件清单
│   ├── Kconfig.projbuild         # 项目 Kconfig
│   ├── Audio/                    # 【音频需求】
│   ├── Iot/                      # 【物联网需求】
│   ├── light_control/            # 【灯光控制需求】
│   └── sensor_control/          # 【传感器控制需求】
├── components/                   # 自定义组件（BluFi、音频板级）
├── Audio_files/                  # 音频资源（打包至 SPIFFS）
├── partitions_dld.csv            # 分区表
└── sdkconfig.defaults            # 默认 sdkconfig
```

---

## 3. 分层与设计理念

### 3.1 通用分层

各需求域内部采用一致的分层思路：

| 层级 | 职责 | 典型实现 |
|------|------|----------|
| **协调/入口层** | 初始化顺序、模块组装、事件路由 | `light_control.c`、`sensor_control.c`、`app_main.c` |
| **业务逻辑层** | 状态机、业务规则、定时与策略 | `light_manager.c`、`sensor_control.c`、`constant_light_control.c` |
| **硬件抽象层 (HAL)** | 外设访问封装，与具体硬件解耦 | `light_control/hardware/`、`sensor_control/hardware/` |

### 3.2 面向对象式 C 的约定

- **封装**：头文件只暴露不透明类型（如 `light_manager_t*`），实现文件内定义完整结构体。
- **生命周期**：通过 `create()` / `destroy()` 管理对象，遵循“谁创建谁销毁”。
- **状态与接口**：对象内部维护状态，对外仅提供 getter/setter 与业务接口。
- **事件驱动**：通过函数指针回调（如 `light_change_callback_t`、`on_button_event`）解耦模块。

### 3.3 跨模块协作方式

- **设备参数**：`device_params` 单例 + NVS（`settings`）作为**中心化参数存储**，各模块通过 Getter/Setter 读写，保证持久化与一致性。
- **IoT 与业务**：MQTT 命令经 `param_handler` 注册表分发到各域的 `*_param_handler`，实现「协议 → 业务」的映射。
- **灯光与传感器**：`sensor_control` 通过 `light_manager` 的接口（如调暗、关灯）和 `light_manager_register_change_callback` 与灯光模块协作。

---

## 4. 各需求域详细架构

### 4.1 音频需求（`main/Audio/`）

| 文件/目录 | 层级 | 职责 |
|-----------|------|------|
| `mp3_player.c/h` | 播放器层 | 基于 ESP-ADF 的 MP3 播放，SPIFFS 读文件，I2S 输出；音量与播放状态查询 |
| `audio_queue.c/h` | 队列管理层 | 多级优先级（URGENT/HIGH/NORMAL/LOW）、按类型防抖、单次/循环播放、背景音乐暂停/恢复 |
| `audio_mode.c/h` | 模式切换层 | 本地模式（MP3 + audio_queue）与 A2DP 蓝牙模式互斥切换 |
| `a2dp_sink.c/h` | 蓝牙音频层 | A2DP Sink 实现，连接/断开/音频启停回调，音量控制 |
| `tts_list.h` | 资源定义 | 语音/提示音文件路径宏，统一管理资源名 |

**数据流**：  
按键/MQTT 等触发 → `audio_queue_play()` / `audio_queue_play_loop()` → 按优先级与防抖入队 → MP3 管道（或 A2DP 流）。  
模式切换：`audio_mode_switch_to_a2dp()` / `audio_mode_switch_to_local()` 负责停止一方、启动另一方。

**依赖**：  
- MP3 播放器会初始化 I2C 总线（音频 Codec），因此 **必须在 `sensor_control_init()` 之前** 完成 `mp3_player_init()`，以便 BH1750 使用同一 I2C。

---

### 4.2 物联网需求（`main/Iot/`）

| 文件/目录 | 层级 | 职责 |
|-----------|------|------|
| `mqtt_manmager.c/h` | 连接与会话 | MQTT 客户端生命周期、订阅/发布、连接状态与重连 |
| `protocol.c/h` | 协议构建 | 设备端上报 JSON 构建（心跳、遗嘱、同步、参数同步、响应等） |
| `protocol_parse.c/h` | 协议解析 | 服务端下发 JSON 解析（获取/设置参数、解绑等），调用对应业务接口 |
| `param_handler.c/h` | 参数调度 | 注册表：参数 key → 处理函数；`param_handler_process` / `param_handler_process_multi` 分发 |
| `device_param_handler.c/h` | 设备参数处理 | 处理设备级 MQTT 参数（音乐、语音、恒光、PIR、定时等），读写 `device_params` |
| `light_param_handler.c/h` | 灯光参数处理 | 处理 lighting/therapy 等 MQTT 参数，调用 `light_manager_set_light_state_percent` / `light_manager_set_therapy_state` |
| `sensor_param_handler.c/h` | 传感器参数处理 | 处理 PIR、恒光、dim_timeout、off_timeout 等，调用 sensor_control / device_params |
| `ota.c/h` | 应用服务 | OTA 固件升级（HTTPS） |
| `sntp_service.c/h` | 应用服务 | SNTP 时间同步 |

**数据流**：  
- **下行**：MQTT 消息 → `protocol_parse` 解析 CMD/参数 → `param_handler_process`（或 multi）→ 根据 key 调用已注册的 `*_param_handler` → 写 `device_params` 或调用 `light_manager` / `sensor_control`。  
- **上行**：业务或定时触发 → `protocol.c` 构建 JSON → `mqtt_manmager` 发布。

**设计要点**：  
- 协议与业务解耦：协议层只做 JSON 编解码与命令分发，具体动作在各自 handler 中完成。  
- 各 `*_param_handler` 需在 `app_main` 中通过 `param_handler_register()` 注册，否则 MQTT 无法路由到对应逻辑。

---

### 4.3 灯光控制需求（`main/light_control/`）

| 文件/目录 | 层级 | 职责 |
|-----------|------|------|
| `light_control.c/h` | 协调层 | 启动/停止灯光模块、LEDC 与灯光/按键管理器创建与初始化、按键回调中调用 light_manager 并触发 MQTT 通知 |
| `light_manager.c/h` | 业务逻辑层 | 四路灯光状态（环境光/下光/上光/红光）、五档亮度、红光三态（OFF/NORMAL/THERAPY）、组合键逻辑、状态变化回调、MQTT 用百分比接口 |
| `button_manager.c/h` | 业务逻辑层 | 多路按键扫描、消抖、事件识别（PRESSED/SINGLE_CLICK/LONG_PRESS），FreeRTOS 任务驱动 |
| `light_param_handler.c/h` | MQTT 对接 | 将 MQTT lighting/therapy 参数转为对 light_manager 的调用（含状态比较优化，避免多余硬件操作） |
| `hardware/ledc_init.c/h` | 硬件抽象 | PWM 通道与占空比/频率控制（红光 5kHz/40Hz 等） |
| `hardware/touch_button.c/h` | 硬件抽象 | 触摸按键 GPIO 读取与编码 |

**灯光 ID 与协议映射**：  
- 协议灯 1 → `LIGHT_ID_UPPER`（上发光板）  
- 协议灯 2 → `LIGHT_ID_LOWER`（下发光板）  
- 协议灯 3 → `LIGHT_ID_AMBIENT`（氛围灯）  
- 红光/光疗 → `LIGHT_ID_RED`，模式由 `therapy_state_t`（OFF/护眼/专注）控制。

**按键与 light_manager 的绑定**：  
在 `light_control.c` 的 `on_button_event()` 中：按键 0–4 映射亮度档位，按键 5 为环境光+下光组合循环，按键 6 为红光模式循环，按键 7 为上光开关；部分长按（如 5 秒切 A2DP、10 秒恢复出厂）也在该回调中处理。

**设计要点**：  
- 状态比较：MQTT 设置时先读当前状态与亮度，仅在有变化时调用 `light_manager_set_light_state_percent` / `light_manager_set_therapy_state`，减少硬件与回调抖动。  
- 灯光变化通过 `light_manager_register_change_callback` 通知 sensor_control（PIR/恒光）与 MQTT 上报。

---

### 4.4 传感器控制需求（`main/sensor_control/`）

| 文件/目录 | 层级 | 职责 |
|-----------|------|------|
| `sensor_control.c/h` | 协调与 PIR 逻辑 | 初始化 PIR 与恒光、人体检测与定时器（dim_timeout / off_timeout）、亮度变化来源标识、MQTT 同步策略、PIR 调暗时亮度记忆与恢复 |
| `constant_light_control.c/h` | 恒光业务 | BH1750 采样、5 分钟基准建立（中位数滤波）、±10% 容差调节灯板亮度、与 PIR 协调（suspend/resume） |
| `sensor_param_handler.c/h` | MQTT 对接 | PIR/恒光开关、dim_timeout、off_timeout 等参数的解析与写入 device_params / sensor_control |
| `hardware/pir_init.c/h` | 硬件抽象 | PIR 传感器 GPIO 初始化与状态读取 |
| `hardware/bh1750.c/h` | 硬件抽象 | BH1750 光照传感器 I2C 驱动 |

**PIR 逻辑概要**：  
- 有人移动 → 重置无人体定时器。  
- 无人体超过 dim_timeout → 调暗到固定档位（如 10%），并记录当前亮度到 `memory_brightness_level`。  
- 再超过 off_timeout → 先通过 settings 将记忆亮度写回 NVS，再关灯，避免把“调暗后的 10%”当成用户设定保存。  
- 亮度变化来源（外部/按键/MQTT、PIR、恒光）用于区分是否触发 `on_light_change` 与 MQTT 同步（例如 PIR 调暗不同步，关灯同步）。

**恒光与 PIR 协调**：  
- PIR 调暗时调用 `constant_light_suspend()`，恢复时 `constant_light_resume()`。  
- 外部改变灯光时调用 `constant_light_on_light_change_external()` 重新建立基准。

**依赖**：  
- BH1750 使用 I2C，与音频 Codec 共享总线，故 **sensor_control 初始化必须在 mp3_player_init() 之后**。

---

### 4.5 其余系统（`main/` 根目录）

| 文件 | 职责 |
|------|------|
| `app_main.c` | 默认事件循环、NVS 初始化、各模块初始化顺序、param_handler 注册、周期堆统计等 |
| `board_pins.h` | 灯光 PWM、触摸按键、LED、蜂鸣器、PIR、I2C 等 GPIO 集中定义 |
| `device_params.c/h` | 设备参数单例：从 NVS 加载、提供 Getter/Setter；被灯光、传感器、音频、IoT 等共同使用 |
| `settings.c/h` | NVS 封装：`settings_start` / `settings_end`、按命名空间读写字符串/整型/布尔，脏数据自动提交 |
| `system_info.c/h` | 系统信息（SN、MAC、设备名等）的初始化与查询 |
