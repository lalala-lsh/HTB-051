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

