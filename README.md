# 家庭室内多节点烟雾报警系统

> 三块 **CH573F** 蓝牙节点 + **MQ 气敏传感器** + **ESP32 网关** + **云端网页大屏**
> 一套完整跑通的物联网实物工程：本地判警、断网照响、上云可视、可远程消音。

<p align="left">
  <img alt="MCU" src="https://img.shields.io/badge/MCU-CH573F%20(RISC--V)-0b7285">
  <img alt="Gateway" src="https://img.shields.io/badge/Gateway-ESP32--S3%20%7C%20ESP32--2432S028R-0ea5e9">
  <img alt="Link" src="https://img.shields.io/badge/Link-BLE%20%2B%20WiFi%20%2B%20MQTT-0d9f6e">
  <img alt="UI" src="https://img.shields.io/badge/UI-LVGL%208.4%20%7C%20Web%20%2B%20ECharts-d98207">
</p>

---

## 它能做什么

| 能力 | 说明 |
|---|---|
| 🚨 **本地判警，断网照响** | 报警判定在 CH573F 节点内完成，不依赖网关、WiFi 或服务器 |
| 📡 **BLE 星型组网** | 一个网关同时连多个节点，双向通信（上报遥测 + 下发指令） |
| 🖥️ **两种网关，任选其一** | 无外设版（ESP32-S3，纯转发）／带外设版（触摸屏，本地大屏 + 声音） |
| 📈 **云端实时大屏** | 实时数值、Δ% 曲线、报警次数统计、事件流，可下发**消音 / 自检 / 复位** |
| 🎛️ **产品化交互**（带屏版） | 四个页签、等级与指数、趋势箭头、报警弹窗、预热倒计时、夜间自动降背光 |
| 🧪 **探头自检 + 一键标定**（带屏版） | 探头没接/掉线会显式报"故障"，而不是安静地显示"正常"（避免假安全） |
| 🔁 **自适应基线** | 节点自己跟踪清洁空气基线，缓慢漂移自动吸收，不用频繁手动标定 |

---

## 一、系统架构

```
  ┌───────────────────────────┐
  │ CH573F 节点 ×3            │   BLE Notify：16 字节二进制遥测帧
  │ MQ-2 / MQ-135 / MQ-137    │   正常 2s 一条，报警 0.5s 一条
  │ + 有源蜂鸣器               │ ──────────────┐
  │ （本地判警，断网照响）      │               │
  └───────────────────────────┘               ▼
                                  ┌────────────────────────────┐
                                  │  网关（两条路线，功能对等）  │
                                  │  A：ESP32-S3（无外设）      │
                                  │  B：ESP32-2432S028R（带屏） │
                                  │  ① 扫描/连接节点            │
                                  │  ② 校验/解析帧              │
                                  │  ③ 转 JSON 上云             │
                                  │  ④ 转发云端指令             │
                                  └──────────────┬─────────────┘
                                        WiFi / MQTT (QoS1)
                                                 ▼
                     ┌───────────────────────────────────────────┐
                     │  服务器（Linux + Docker 可选）             │
                     │  ├ Mosquitto MQTT Broker（1883）          │
                     │  ├ 后端 app.py（HTTP :3000 + SSE）         │
                     │  ├ SQLite（历史曲线 / 事件 / 设备表）      │
                     │  └ 可选 Cloudflare 隧道（HTTPS 外网访问）  │
                     └──────────────────┬────────────────────────┘
                                        │ HTTPS
                                  手机 / 电脑浏览器
```

**为什么用 BLE 主从星型，而不是 Mesh / 纯广播？**

| 方案 | 结论 |
|---|---|
| **BLE 主从（ESP32 做 Central）** | ✅ **本工程采用**。CH57x 协议栈本身就是外设角色；每条链路双向可靠（上报 + 下发） |
| BLE Mesh | ❌ 沁恒 CH57x 协议栈不提供 Mesh 模型（无 provisioning/relay/元素模型），等于重写协议栈 |
| 纯广播 | ❌ 广播包无链路层确认、丢包无法补救；且**无法下发**消音/自检指令 |

---

## 二、硬件清单

| # | 部件 | 型号 / 规格 | 数量 | 备注 |
|---|---|---|---|---|
| 1 | 蓝牙节点主控 | **CH573F** 最小系统板（RISC-V，BLE 5.0） | 3 | 每一块烧不同 `NODE_TAG` |
| 2 | 气体传感器模块 | **MQ-2** / **MQ-135** / **MQ-137** | 各 1 | 见下方"传感器含义" |
| 3 | 有源蜂鸣器 | 3.3V 有源（低电平触发） | 3 | 节点本地报警声 |
| 4 | 网关 A | **ESP32-S3** 开发板 | 1 | 无屏，纯转发 |
| 5 | 网关 B | **ESP32-2432S028R**（CYD，2.8" 240×320 电阻触摸） | 1 | 本地大屏 + 扬声器 |
| 6 | 扬声器 / 功放 | 板上自带功放（IO26） | 1 | 接扬声器才能听到带屏版的报警声 |
| 7 | 传感器供电 | 5V（模块 VCC） | — | MQ 加热丝额定 5V，3.3V 会导致读数偏低 |

### 三种传感器在屏幕/网页上表达的数据含义

| 型号 | 主要检测对象 | 判据类型 | 数据怎么读 |
|---|---|---|---|
| **MQ-2** | 可燃气 / 烟雾（LPG、甲烷、丙烷、氢气、烟） | **安全类** | 看 **Δ%**：负值越大 = 传感器电阻越小 = 浓度越高；≥25% 触发节点报警 |
| **MQ-135** | 综合空气质量（NH₃、苯系物、烟等） | **环境类** | 看 **Rs/R0 指数**（R0 为清洁空气基线）：指数越高空气越好 |
| **MQ-137** | 氨气 NH₃ | **环境类** | 同上 |

> 两支环境类传感器给出的是"相对清洁空气的好坏程度"，不报"报警"；安全类才会触发蜂鸣器和红色报警。
> 带屏版固件会**自动按型号切换显示语义**，卡片上直接写明"当前传感器 + 这行数字什么意思"。

---

## 三、★ 两条网关路线（请勿混淆）

两条线**功能对等**：都是 BLE 主机 + WiFi/MQTT + SNTP + 离线缓存，任选一条都能跑通整套系统。
区别只在"有没有本地人机界面和外设"。

### 3.1 A 线：`gateway_esp32s3/` —— 无外设版

| 项目 | 内容 |
|---|---|
| 芯片 | ESP32-S3（ESP-IDF v5.1.2） |
| 外设 | **无**。没有屏幕、没有触摸、没有蜂鸣器、没有按键 |
| 代码量 | **4 个源文件**：`gateway_main.c` / `wifi_mqtt.c` / `node_proto.h` / `app_config.h` |
| 适用 | 只想把数据送上云；想先跑通链路；内存宽裕、调试最简单 |
| 资源 | 内存充裕（双核 + 更大 RAM），BLE 扫描与连接互不干扰 |

**这是工程的"基线版本"，所有协议定义以它为准。**

### 3.2 B 线：`cyd_gateway/` —— 带触摸屏外设版

| 项目 | 内容 |
|---|---|
| 芯片 | **ESP32-WROOM-32E**（经典 ESP32，**单射频**，4 MB Flash） |
| 板型 | **ESP32-2432S028R**（俗称 Cheap Yellow Display / CYD） |
| 外设 | **ILI9341 240×320 LCD + XPT2046 电阻触摸 + 扬声器功放 + RGB LED + 光敏电阻** |
| GUI | **LVGL 8.4**（双 24 行缓冲），开机自检页 + 四个页签 |
| 代码量 | A 线 4 个文件 + **9 个新增文件** + main 大幅扩展（`ui.c` 约 1500 行） |
| 适用 | 想在设备本地就能看数据、听报警声、不依赖手机 |

**它是在 A 线基础上移植过来的**，不是另写一套。

#### 引脚定义（`main/board_config.h`）

| 外设 | 引脚 |
|---|---|
| LCD（SPI2/HSPI） | SCLK 14 · MOSI 13 · MISO 12 · CS 15 · DC 2 · BL 21 |
| 触摸 XPT2046（SPI3/VSPI） | SCLK 25 · MOSI 32 · MISO 39 · CS 33 · IRQ 36 |
| 扬声器/功放 | IO26 |
| RGB LED | R 4 · G 16 · B 17 |
| BOOT 键 / 光敏 | IO0 / IO34 |

#### 界面结构（四个页签）

| 页签 | 内容 |
|---|---|
| **监控** | 每个节点一张卡片：名称/型号/等级徽章、大号指数、Δ% 色条、趋势箭头 ↑→↓、ADC / AO 电压 / Rs / 信号；点卡片进入**曲线详情页** |
| **记录** | 报警与状态事件的时间线（本地环形缓存） |
| **控制** | 节点选择、消音 / 自检 / 复位、**清洁空气一键标定**、阈值查看 |
| **设置** | 背光亮度滑条、夜间自动降背光、字体与版本信息等 |

另有**开机自检页**（启动时逐项打勾，3.5 秒后自动进入主界面）和**报警自动弹窗**（全屏红色呼吸 + 消音/关闭按钮）。

### 3.3 两条线对照表

| 维度 | A 线（ESP32-S3） | B 线（CYD / ESP32） |
|---|---|---|
| 本地屏幕 | ❌ | ✅ 2.8" 240×320 |
| 触摸交互 | ❌ | ✅ 电阻触摸 |
| 本地报警声 | ❌（只有节点蜂鸣器） | ✅ 板上功放 + 扬声器（嘀嘀嘀-停 节奏） |
| 本地标定 | ❌（只能靠云端/改代码） | ✅ 控制页一键标定，参数存 NVS |
| 探头自检 | 仅日志提示 | ✅ 卡片直接显示"故障 / 请检查探头接线" |
| 射频 | 双核，BLE 与 WiFi 共存宽裕 | **单射频**，扫描与 WiFi 必须分时（见 3.5） |
| IDE / 工具链 | ESP-IDF | ESP-IDF + LVGL |
| 建议场景 | 数据上云、做二次开发 | 需要脱机演示、答辩现场效果 |

### 3.4 B 线相对 A 线**逐文件**改了什么

| 文件 | A 线 | B 线 | 说明 |
|---|---|---|---|
| `node_proto.h` | ✅ | ✅ **逐字节相同** | 协议定义，两侧必须一致 |
| `wifi_mqtt.c/.h` | ✅ | ✅ 基本相同 | B 线额外加了 `WifiMqtt_GetIp()` / `GetWifiRssi()` 给屏幕用 |
| `app_config.h` | ✅ | ✅ 有差异 | B 线多了探头自检阈值等配置 |
| `gateway_main.c` | ✅ 基线 | ⚠️ **多处带 `[CYD port]` 注释的改动** | 单射频调度、LVGL 任务、标定、NVS 等 |
| `board.c/.h`、`board_config.h` | ❌ | ✅ **B 线独有** | 引脚与板级初始化 |
| `display.c/.h` | ❌ | ✅ **B 线独有** | LCD + 触摸 + LVGL 移植层 |
| `ui.c/.h` | ❌ | ✅ **B 线独有** | 全部界面（约 1500 行） |
| `gw_buzzer.c/.h` | ❌ | ✅ **B 线独有** | LEDC 蜂鸣器 |
| `gw_cal.c/.h` | ❌ | ✅ **B 线独有** | NVS 标定存储（按 MAC 索引） |
| `gw_hist.c/.h`、`gw_log.c/.h`、`gw_view.h` | ❌ | ✅ **B 线独有** | 事件缓存、串口日志、视图数据结构 |

> **给二次开发者的建议**：改协议只改 `node_proto.h`（两侧同步）；改业务逻辑优先动 A 线再同步到 B 线，
> 因为 B 线的复杂度几乎全在"外设 + 单射频调度"上。

### 3.5 B 线移植时必须改的一处 BLE 逻辑（经典 ESP32 单射频）

ESP32-S3 是双核、BLE 与 WiFi 共存宽裕；**经典 ESP32 只有一个射频**，
BLE 扫描/连接与 WiFi 会互相抢时隙。A 线的"先长时间扫描、再去连"的写法在 CYD 上会出现**反复断连**。

B 线的做法：

1. **只在启动阶段扫描**，之后停止扫描，仅维护已有连接；
2. 扫描与 MQTT 重连、以及 LVGL 刷新错峰进行；
3. 连接参数更新（4 秒一次）改到 MQTT 空闲窗口再发；
4. 报警期间节点上报从 2 秒变 0.5 秒，**单射频下这是最紧张的时刻**——B 线在此处做了节流。

这一节的完整定位过程（含一次真实的崩溃：在临界区里调用了 `localtime_r()`）
写在 [`cyd_gateway/README.md`](cyd_gateway/README.md) 第十一节，是很好的排错参考。

---

## 四、节点固件（CH573F）

### 4.1 组成与接线

节点实物就三样：**CH573F 最小系统板 + MQ 传感器模块 + 有源蜂鸣器**（没有 OLED、没有按键）。

| 信号 | 引脚 | 说明 |
|---|---|---|
| AO（模拟输出） | **PA15**（ADC 通道 AIN5） | 默认按 PA4 编译，可用 `-AoPin` 指定 |
| DO（数字输出） | **PA5** | 板上比较器输出，**只用于显示，不参与判定** |
| 蜂鸣器 | PB15 | 有源蜂鸣器 |
| 调试串口 | PB4 / PB7 | 115200，用来打印实时数据 |

> ⚠️ **注意 PA4 与 PA15 的区别**：CH573F 的 ADC 通道与引脚是固定绑定的
> （AIN0→PA4，AIN5→PA15…）。接错引脚会读到悬空的电源轨（≈3.3V），
> 表现就是"数值恒定不变"。工程提供 `-AoPin` 参数自动带出正确通道号。

### 4.2 判警算法

```
Rs   = RL × (VCC − Vao) / Vao                 // 传感器等效电阻
Δ%   = (Rs / Rs0 − 1) × 100                  // 相对自适应基线的变化率
```

- **自适应基线 `Rs0`**：只允许缓慢跟随（每样本 ≤1%），且**只向"更干净"方向更新**，避免把污染当成新基线；
- **报警**：`|Δ%| ≥ 25%` 连续 2 次；**解除**：`< 12%` 连续 5 次（带迟滞，避免抖动反复）；
- **预热**：上电后 30 秒内只上报不判警，帧里带 `warmup` 位；
- **消音**：`M`/`M60` 只停本机蜂鸣器，判定与上报照常。

### 4.3 关键配置项（`APP/include/node_cfg.h`）

| 宏 | 默认 | 含义 |
|---|---|---|
| `NODE_TAG` | `N01` | 节点编号（显示用，建议每块不同） |
| `NODE_MODEL` | `MQ-2` | 传感器型号（决定屏幕上的数据语义） |
| `NODE_MODULE_VCC_MV` | `3300` | 模块供电电压（5V 供电要改成 5000） |
| `NODE_AO_DIV_X100` | `100` | AO 分压比 ×100（10k/10k 分压填 `200`） |
| `NODE_RL_OHM` | `1000` | 模块负载电阻 |
| `NODE_AO_CH` / `NODE_AO_PIN` / `NODE_AO_PIN_NAME` | AIN0 / PA4 | 受 `#ifndef` 保护，可用编译参数覆盖 |

### 4.4 一次编译，给多块板出不同固件

`tools/build_node.ps1` 支持在不改源码的情况下覆盖身份与接线参数：

```powershell
# 三块板各出一份（AO 都在 PA15；5V 供电 + 10k/10k 分压）
.\tools\build_node.ps1 -Tag N01 -Model "MQ-135" -AoPin PA15 `
    -Define NODE_MODULE_VCC_MV=5000,NODE_AO_DIV_X100=200 -Out .\deploy\fw\N01_MQ-135.hex
.\tools\build_node.ps1 -Tag N02 -Model "MQ-137" -AoPin PA15 `
    -Define NODE_MODULE_VCC_MV=5000,NODE_AO_DIV_X100=200 -Out .\deploy\fw\N02_MQ-137.hex
.\tools\build_node.ps1 -Tag N03 -Model "MQ-2"   -AoPin PA15 `
    -Define NODE_MODULE_VCC_MV=5000,NODE_AO_DIV_X100=200 -Out .\deploy\fw\N03_MQ-2.hex
```

`-AoPin` 会自动带出对应 ADC 通道号（PA4/PA5/PA8/PA9/PA12/PA13/PA14/PA15），
因为 CH573 的通道与引脚是固定绑定的，写错一个另一个也一定错。

烧写：WCHISPTool → 芯片选 `CH573` → 选对应 hex → 下载。

---

## 五、通信协议

### 5.1 BLE（节点 ↔ 网关）

角色：节点 = Peripheral（广播名 `SMOKE_NODE`），网关 = Central。
GATT 沿用沁恒 SimpleProfile（服务 `0xFFE0`）：

| 特征 | UUID | 方向 | 用途 |
|---|---|---|---|
| CHAR3 | `0xFFE3` | 网关 → 节点（Write） | 下发命令（ASCII，≤20 字节） |
| CHAR4 | `0xFFE4` | 节点 → 网关（Notify） | 遥测帧 / 信息帧 |

> 默认 MTU 23 → 单次负载只有 **20 字节**，所以遥测用**二进制**而不是 JSON。

**遥测帧（16 字节，小端）**

| 偏移 | 长度 | 字段 | 说明 |
|---|---|---|---|
| 0 | 1 | magic | 固定 `0x53`（`'S'`） |
| 1 | 1 | type | `0x01` 遥测 |
| 2 | 1 | seq | 序号，用于粗查丢包 |
| 3 | 1 | flags | bit0 报警 · bit1 预热 · bit2 DO 高 · bit3 已消音 |
| 4 | 2 | adc | ADC 原始值 0~4095 |
| 6 | 2 | ao_mv | 模块 AO 电压（mV） |
| 8 | 4 | rs | 等效电阻 Rs（Ω） |
| 12 | 2 | dpct | Δ% ×10（有符号，`-523` = −52.3%） |
| 14 | 1 | do | DO 电平 |
| 15 | 1 | sum | 前 15 字节求和取低 8 位 |

**信息帧**：`0x53` + `0x02` + 18 字节 ASCII `"TAG|MODEL|FW"`（如 `N01|MQ-2|1.0`），连上后发一次，之后每 30 秒补发。

**下行命令**：`M`/`MUTE`（消音 60s）、`M30`（消音 30s）、`T`/`TEST`（自检）、`R`/`RESET`（清报警锁存）。

### 5.2 MQTT（网关 ↔ 云）

前缀 `home/<gateway_id>`，`<MAC>` 为 12 位十六进制：

| 主题 | 方向 | retain | 说明 |
|---|---|---|---|
| `home/gw01/tele/<MAC>` | 上行 | 否 | 遥测（约 2s 一条） |
| `home/gw01/event/<MAC>` | 上行 | 否 | 报警 / 解除事件 |
| `home/gw01/state/<MAC>` | 上行 | **是** | 节点在线状态 + 编号/型号/版本 |
| `home/gw01/state/gateway` | 上行 | **是** | 网关心跳（IP / WiFi / 在线节点数） |
| `home/gw01/ack/<MAC>` | 上行 | 否 | 指令回执 |
| `home/gw01/cmd/<MAC>` | 下行 | 否 | 控制指令（纯文本） |

- 状态类用 **retain**：网页任何时候打开都能立刻知道谁在线；
- 网关注册 **LWT 遗嘱** `{"online":false}`：异常掉电时 Broker 立刻代发离线消息；
- **ACL 最小权限**：设备只能写自己的上报主题、只能读命令主题；后端能读全部、只能写命令主题。

遥测 JSON 示例：

```json
{"ts":1790933403,"mac":"AABBCCDDEEFF","seq":12,"adc":1035,"ao":1668,
 "rs":1998,"dpct":-52.3,"alarm":1,"warmup":0,"do":0,"muted":0,"rssi":-45}
```

### 5.3 HTTP / SSE（网页 ↔ 后端）

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/status` | 网关状态 + 全部节点当前值 |
| GET | `/api/series?mac=&hours=&since=&max_points=&fmt=` | Δ% 曲线；`since` 取增量；点太多自动抽稀；`fmt=2` 返回紧凑数组 |
| GET | `/api/events?limit=&mac=` | 事件流 |
| GET | `/api/stats?hours=24` | 每小时报警次数 |
| GET | `/api/stream` | **SSE 实时推送**（`tele`/`alarm`/`state`/`status`） |
| POST | `/api/cmd` | `{"mac":"…","text":"M"}` |

鉴权：`X-Auth: <口令>`（SSE 用 `?token=`）。

---

## 六、云端与网页

| 层 | 技术 | 说明 |
|---|---|---|
| Broker | Mosquitto 2.x | ACL 由安装脚本按 `config.json` 自动生成 |
| 后端 | Python 3 + `ThreadingHTTPServer` + paho-mqtt + SQLite | 无 Web 框架依赖，单文件 `app.py` |
| 前端 | 原生 JS + ECharts 5 | 无构建步骤，`index.html` + `app.js` + `style.css` |
| 部署 | systemd 两个单元 + 可选 Cloudflare 隧道 | 一键安装脚本幂等，数据库不动 |
| 外网 | 可选 Cloudflare 隧道出站连接 | 未备案域名也能 HTTPS 访问 |

**网页性能上做过的实测优化**（都有数据支撑）：

| 优化 | 效果 |
|---|---|
| 后端 gzip | 首屏 **1.16 MB → 383 KB** |
| 曲线点紧凑编码（`fmt=2`，数组代替对象） | 1359 点 **80.4 KB → 30.1 KB（省 63%）** |
| **SSE 遥测消息直接携带数值** | 前端更新卡片不再发 HTTP，请求量从"每帧一次"降到"每 15 秒兜底一次" |
| 前端曲线按 (节点, 时间范围) 缓存 + 空闲预热 | 切换节点**瞬时出图**（有缓存时零请求） |
| 卡片 DOM 就地打补丁、选择器按签名重建 | 按钮不再被反复销毁重建，点击不再"落空" |
| canvas 采样（`lttb`）+ 不做整图 `notMerge` | 切换曲线不再整块重画 |
| 隧道改 1 条 http2 长连接 | 空闲流量 **降到约 12 MB/天** |

---

## 七、仓库结构与开源发布建议

```
.
├─ README.md                     ← 本文件（项目说明）
├─ LICENSE                       ← 建议补上（见"许可"）
├─ .gitignore                    ← 排除 build/ managed_components/ .venv/ data/ logs/
├─ docs\                         ← 设计文档
│   ├─ 01_系统架构与组网方式.md
│   ├─ 02_硬件接线与节点部署.md
│   ├─ 03_通信协议.md
│   ├─ 04_编译烧写与联调步骤.md
│   ├─ 05_答辩要点与常见问题.md
│   └─ 06_运维与部署实操.md      ← 原 README，作者的运维/部署备忘
├─ node_ch573f\                  ← ① 节点固件（MounRiver Studio 工程）
│   ├─ node\HAL  node\LIB        ← 沁恒官方 SDK（请勿改动，勿重新授权）
│   ├─ SRC\                      ← 沁恒 Ld/RVMSIS/Startup/StdPeriphDriver
│   └─ node\SmokeNode\           ← ★ 工程本体（APP 为应用代码，Profile 为 BLE 服务）
├─ gateway_esp32s3\              ← ② 网关 A：无外设版（ESP-IDF）
│   └─ main\                     ← gateway_main.c / wifi_mqtt.c / node_proto.h / app_config.h
├─ cyd_gateway\                  ← ③ 网关 B：带触摸屏外设版（ESP-IDF + LVGL）
│   ├─ main\                     ← 在 A 线基础上 + board/display/ui/gw_* 共 9 个新文件
│   ├─ main\fonts\               ← 中文字库（生成脚本 tools/gen_lvgl_font.py）
│   └─ README.md                 ← 移植与产品化的完整记录
├─ server\                       ← ④ 云端：Mosquitto 配置 + 后端 + 网页 + 部署脚本
│   ├─ config.example.json       ← 配置模板（复制成 config.json 再改口令）
│   └─ config.local.json         ← 你的真实配置（**已被 .gitignore 排除，不会公开**）
├─ deploy\                       ← 打包 / 一键安装 / 流量诊断
└─ tools\                        ← 编译与环境脚本（build_node / build_gateway / check_env …）
```

> 两条网关路线都在同一个仓库里（`gateway_esp32s3/` 与 `cyd_gateway/`），
> 差异可以直接用 `diff` 看清；`managed_components/`、`build/`、`.venv/`、`data/`、`logs/`
> 全部进 `.gitignore`（LVGL 等依赖由 `idf.py` 按 `idf_component.yml` 自动拉取）。
>
> **关于配置与口令**：公开仓库里的 `server/config.json`、网关固件里的 WiFi/MQTT 信息
> 都是占位符（`CHANGE_ME_*` / `YOUR_WIFI_SSID` / 文档用 IP `192.0.2.10`）。
> 你自己的真实值放在 `server/config.local.json`（已 gitignore）；
> `deploy/pack_for_upload.ps1` 打包时会自动优先使用它，避免把占位符部署到线上。

---

## 八、编译与烧写

| 目标 | 工具链 | 命令 |
|---|---|---|
| 节点 | MounRiver Studio 2（RISC-V GCC） | `.\tools\build_node.ps1 -Tag N01 -Model "MQ-135" -AoPin PA15 -Out .\deploy\fw\N01_MQ-135.hex` |
| 网关 A | ESP-IDF v5.1.2 | `idf.py build` / `idf.py -p <COM> flash monitor` |
| 网关 B | ESP-IDF v5.1.2 + LVGL 8.4 | 同上（依赖由 `main/idf_component.yml` 自动拉取） |
| 服务器 | Python 3 / mosquitto | 本地：`server\setup.ps1` → `server\start.ps1`；云：见 `deploy/install_on_server.sh` |
| 中文字库再生成（改文案后） | Python + lv_font_conv | `python tools/gen_lvgl_font.py`（CYD 工程内） |

环境自检：`.\tools\check_env.ps1`。

---

## 九、实测数据

| 指标 | 实测值 |
|---|---|
| 节点上报 | 正常 2 s / 报警 0.5 s；单帧 16 字节 |
| BLE 丢包 | 序号连续，长时间运行无明显丢帧（`seq` 可自查） |
| 网关 A 连接数 | 默认 4（`APP_MAX_NODES`，需同步 `CONFIG_BT_ACL_CONNECTIONS`） |
| CYD 固件体积 | app 约 2.19 MB，3 MB 分区剩余约 30% |
| CYD 内存 | 空闲堆稳定 **30 KB 以上**，历史最低约 24 KB（双 24 行 LVGL 缓冲） |
| CYD 界面 | 4 页签 + 曲线详情 + 报警弹窗，切换无明显卡顿 |
| 网页首屏 | gzip 后 383 KB（ECharts 占大头） |
| 曲线接口 | 1359 点 30 KB；增量请求约 170 字节 |
| 隧道流量 | 约 12 MB/天（1 条 http2 长连接） |

---

## 十、环境类判据：为什么不用 Δ% 而要标定 R0

安全类（MQ-2）关心"现在比刚才差多少"，用自适应基线 Δ% 最灵敏；
环境类（MQ-135 / MQ-137）关心"现在空气好不好"，需要一个**绝对参考**：

```
R0    = 清洁空气下测得的 Rs（控制页一键标定，按 MAC 存进 NVS）
指数  = clamp( (100 − Rs/R0 × 100) × 100 / (100 − ratio_min), 0, 100 )   // ratio_min = 30%
```

等级：优 ≥86 · 良 74~86 · 轻度 58~74 · 中度 44~58 · 重度 <44。
**未标定的环境节点会显式提示"未标定"**，不会拿一个没意义的指数糊弄用户。

---

## 十一、踩坑与设计取舍（这部分对复刻的人最有用）

| 问题 | 现象 | 结论 / 处理 |
|---|---|---|
| **经典 ESP32 单射频** | 报警期间（0.5 s 上报）反复断连 | 扫描只在启动阶段做；扫描/MQTT/LVGL 错峰；见 3.5 |
| **临界区里调 `localtime_r()`** | 高负载下偶发崩溃（看门狗） | 时钟格式化移出临界区，改用预格式化缓存 |
| **ADC 通道与引脚绑定** | 读数恒定贴在电源轨（假安全） | CH573 通道↔引脚固定，接错脚读悬空轨；用 `-AoPin` 统一 |
| **探头没接被判"正常"** | 悬空→贴轨→Rs 极小→Δ% 被基线拉到 0 | 加"滑动窗口 + 贴轨"探头自检，显式报**故障** |
| **假恢复** | 单次 ±1 计数抖动就打印"恢复正常" | 判据改成窗口内 max−min ≤ 1，不再被单点抖动骗 |
| **中文路径 + ESP-IDF** | `UnicodeDecodeError: 'gbk'` | 工程放纯英文路径 + 预设 `PYTHONUTF8` |
| **PowerShell 5.1 吃引号** | `-DTAG=N01` 被当未声明标识符 | 参数里用 `\"` 转义形式传递 |
| **PS 脚本 BOM** | 中文注释导致 ParserError | `.ps1` 存 UTF-8 **带 BOM** |
| **CDN 缓存** | 域名访问是旧界面、IP 直连是新界面 | `?v=` 版本号 + 后端 `Cache-Control` |
| **隧道流量** | 空闲也几十~几百 MB/天 | 改 1 条 http2 连接 → 约 12 MB/天 |

更详细的过程记录见 `docs/04`、`docs/05`、`cyd_gateway/README.md`、`deploy/README_部署到阿里云.md`。

---

## 十二、已知限制与 Roadmap

**已知限制**

- MQ 传感器需要**预热**（分钟级），且对环境温湿度敏感；绝对精度不适合做计量，适合做趋势与阈值报警；
- MQ 模块**必须 5V 供电**（加热丝额定），AO 超过 ADC 量程时要加分压并相应设置 `NODE_MODULE_VCC_MV` / `NODE_AO_DIV_X100`；
- 带屏版是**单射频**，节点数量上去后建议改用 ESP32-S3 或 ESP32-C6；
- 事件记录目前是内存环形缓存，**掉电不持久**（刻意没做，避免频繁写 flash）；
- 标定值存在网关 NVS，换网关需要重新标定。

**Roadmap（欢迎 PR）**

- [ ] 节点新增 `B`/`BASELINE` 命令：不改基线就能重取基线（省得断电等待）
- [ ] 环境类超标告警通道（分级预警 + 云端统计）
- [ ] 标定值上传云端，换网关自动同步
- [ ] 设置项持久化（亮度/夜间模式写 NVS）
- [ ] 把"探头自检"的滑动窗口参数做成网页可配

---

## 十三、第三方组件与许可

| 组件 | 用途 | 许可 |
|---|---|---|
| WCH CH573 SDK（`SRC/`、`node/LIB`、`node/HAL`） | 节点外设驱动 + BLE 协议栈 | 沁恒官方 SDK 授权（**请勿重新授权/售卖**） |
| ESP-IDF v5.1.2 | 网关框架 | Apache-2.0 |
| LVGL 8.4 | 触摸屏 GUI | MIT |
| ECharts 5 | 网页图表 | Apache-2.0 |
| Mosquitto | MQTT Broker | EPL-2.0 / EDL |
| paho-mqtt | 后端 MQTT 客户端 | EPL-2.0 / EDL |

**本工程自有代码**（节点 `APP/`、`gateway_esp32s3/main`、`cyd_gateway/main`、`server/`、`tools/`、`deploy/`）
建议采用 **MIT** 许可。

> ⚠️ 正式开源前请**确认许可类型**并补一份 `LICENSE` 到仓库根目录。

**个人信息清理情况**（发布前已做过一轮全仓库扫描）：服务器公网 IP、访问域名、网页口令、
MQTT 账号口令、Cloudflare 隧道 token、家庭 WiFi 的 SSID/密码、示例 MAC、本机用户名与绝对路径
**均已替换为占位符**；你的真实配置只保留在 `server/config.local.json`（已被 `.gitignore` 排除）。

---

## 十四、文档索引

| 文档 | 内容 |
|---|---|
| [docs/01_系统架构与组网方式.md](docs/01_系统架构与组网方式.md) | 为什么选 BLE 星型、整体分层 |
| [docs/02_硬件接线与节点部署.md](docs/02_硬件接线与节点部署.md) | 接线图、节点身份、部署步骤 |
| [docs/03_通信协议.md](docs/03_通信协议.md) | BLE 帧格式、MQTT 主题、HTTP 接口 |
| [docs/04_编译烧写与联调步骤.md](docs/04_编译烧写与联调步骤.md) | 环境搭建、编译、烧写、联调、报错排查 |
| [docs/05_答辩要点与常见问题.md](docs/05_答辩要点与常见问题.md) | 设计取舍、常见提问与回答 |
| [docs/06_运维与部署实操.md](docs/06_运维与部署实操.md) | 原 README：日常运维、部署、排障备忘 |
| [docs/家庭室内多节点烟雾报警系统_设计说明书.docx](docs/家庭室内多节点烟雾报警系统_设计说明书.docx) | 可直接提交的《设计说明书》（Word），配套 [PDF](docs/家庭室内多节点烟雾报警系统_设计说明书.pdf) |
| [tools/build_report.py](tools/build_report.py) | 上面那份说明书的生成脚本（改内容后重新跑一次即可） |
| [cyd_gateway/README.md](cyd_gateway/README.md) | 触摸屏版移植与产品化全过程（阶段 1~4） |
| [server/README.md](server/README.md) | 后端与网页说明 |
| [deploy/README_部署到阿里云.md](deploy/README_部署到阿里云.md) | 云服务器部署手册 |

---

<p align="center"><i>如果这个工程对你有帮助，欢迎 Star / Fork；有问题请提 Issue。</i></p>
