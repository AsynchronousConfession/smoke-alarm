# PC 端服务器（MQTT + 后端 + 网页 + 隧道）

```
CH573F×N ──BLE──▶ ESP32-S3 网关 ──家里WiFi──▶ 笔记本
                                              ├─ Mosquitto    (MQTT 服务器, :1883)
                                              ├─ backend/app.py(订阅入库 + 网页, :3000)
                                              ├─ data/iot.db   (SQLite 历史数据)
                                              └─ cloudflared   (隧道, 出站连接)
                                                         │
                                       手机 ◀──HTTPS── Cloudflare ◀┘
```

笔记本上只需跑三个小程序，全部由 `start.ps1` 一键拉起。不需要 Docker、不需要公网 IP、不用改路由器。

---

## 一、一次性准备

### 1. 装 Mosquitto（MQTT 服务器）

双击 `tools\mosquitto-install-x64.exe`，一路 Next（装到 `C:\Program Files\mosquitto`）。
问"Install as a service"时**不要勾**（我们用脚本自己启动，方便随时停）。

管理员 PowerShell 放行端口（只需一次）：

```powershell
New-NetFirewallRule -DisplayName "MQTT 1883" -Direction Inbound -Protocol TCP -LocalPort 1883 -Action Allow
New-NetFirewallRule -DisplayName "Web 3000"  -Direction Inbound -Protocol TCP -LocalPort 3000 -Action Allow
```

### 2. 跑初始化脚本

```powershell
cd <工程>\server
powershell -ExecutionPolicy Bypass -File .\setup.ps1
```

它会：

* 用系统 Python 建 `.venv`，**离线**安装 `paho-mqtt`（wheel 已经在 `tools\` 里）；
* 按 `config.json` 生成 Mosquitto 的**密码文件、ACL、主配置**。

> ⚠️ **关于目录**：Mosquitto 在 Windows 上不支持中文路径（会报
> `Unable to open pwfile ...` / `Unable to open log file ...` 然后直接退出）。
> setup.ps1 会按工程路径自动选择运行目录：
>   · 工程在纯英文路径（当前 `D:\smoke-alarm\04`）→ 直接用 `server\mosquitto\`，工程自包含；
>   · 工程在中文路径 → 退回到纯英文目录（默认 `D:\A_ESP32_project\smoke_alarm_server`）。
> 选好的位置记在 `server\runtime.json`，`start.ps1` 照它启动。
> 想自己指定：先设环境变量 `SMOKE_SERVER_DIR=D:\某个英文目录`，再重跑 setup.ps1。
> **项目的 `config.json` 始终是唯一配置源**。

### 3. 隧道（已经有域名和 token 就可以跳过）

`config.json` 里的 `cloudflare.tunnel_token` 已经填好。如果换了自己的域名：

1. 域名托管到 Cloudflare（改 NS）；
2. Zero Trust → Networks → Tunnels → Create a tunnel（Cloudflared）→ 复制 token；
3. token 填进 `config.json` 的 `cloudflare.tunnel_token`；
4. Public Hostnames 加一条：`iot.你的域名` → `HTTP` → `localhost:3000`。

> 同一台电脑上**不要同时跑两个用同一个 token 的 `start.ps1`**（两个工程目录各跑一个也不行），
> 否则 Cloudflare 会把请求轮流分发，出现"时而空白"。换目录时先 `.\start.ps1 -Stop`。

---

## 二、每次使用

```powershell
cd <工程>\server
powershell -ExecutionPolicy Bypass -File .\start.ps1          # 启动
powershell -ExecutionPolicy Bypass -File .\start.ps1 -Stop     # 停止
powershell -ExecutionPolicy Bypass -File .\start.ps1 -ShowLog  # 另开窗口看日志
```

启动后会打印：

```
  本机网页 : http://127.0.0.1:3000
  访问口令 : CHANGE_ME_WEB_PW
  本机网卡地址（把 ESP32 所在网络的那个填进 app_config.h 的 APP_MQTT_URI）:
    192.168.137.1    本地连接* 2  <== 移动热点，ESP32 通常用这个
    192.168.1.100      以太网
```

把和 ESP32 同网段的那个地址填进 `gateway_esp32s3\main\app_config.h` 的 `APP_MQTT_URI`。

> ESP32-S3 只支持 2.4GHz：笔记本移动热点必须在"设置 → 网络和 Internet → 移动热点 →
> 网络频带"里选 **2.4GHz**，否则 ESP32 根本搜不到。

---

## 三、不接硬件先验证整条链路

```powershell
cd tools
..\.venv\Scripts\python.exe selftest.py
```

它会扮演一个节点上报遥测和报警事件，再检查后端是否入库，
最后等你 20 秒（这 20 秒内去网页点一次"发送"）验证下行通道。

想连 Mosquitto 都不装、纯离线跑一遍：`..\.venv\Scripts\python.exe mini_broker_test.py`
（自带一个迷你 MQTT broker）。

---

## 四、MQTT 主题与数据格式

| 主题 | 方向 | QoS | retain | 说明 |
|---|---|---|---|---|
| `home/gw01/tele/<MAC>` | 上行 | 1 | 否 | 遥测（约 2 秒一条，报警时 0.5 秒） |
| `home/gw01/event/<MAC>` | 上行 | 1 | 否 | 报警 / 解除事件 |
| `home/gw01/state/<MAC>` | 上行 | 1 | 是 | 节点在线状态 + 编号/型号/版本 |
| `home/gw01/state/gateway` | 上行 | 1 | 是 | 网关心跳（IP/WiFi 信号/在线节点数） |
| `home/gw01/ack/<MAC>` | 上行 | 1 | 否 | 指令回执 |
| `home/gw01/cmd/<MAC>` | 下行 | 1 | 否 | 控制指令（纯文本） |

`<MAC>` 是 12 位十六进制（如 `AABBCCDDEEFF`）。完整字段说明见 `..\docs\03_通信协议.md`。

---

## 五、网页接口（后端 REST）

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/status` | 网关状态 + 全部节点当前值 |
| GET | `/api/series?mac=&hours=` | Δ% 曲线（默认近 3 小时） |
| GET | `/api/events?limit=&mac=` | 事件流 |
| GET | `/api/stats?hours=24` | 每小时报警次数 |
| GET | `/api/stream` | SSE 实时推送 |
| POST | `/api/cmd` | `{"mac":"…","text":"M"}` |

鉴权：请求头 `X-Auth: <访问口令>`（SSE 用 `?token=`）。

---

## 六、数据库

SQLite 文件：`server\data\iot.db`（WAL 模式）。三张表：

| 表 | 用途 |
|---|---|
| `devices` | 节点台账：标签、编号、型号、在线状态、最新数值、累计报警次数 |
| `telemetry` | 曲线数据，**2 秒一个点**（报警跳变时立刻补一条），默认保留 7 天（`tele_keep_days`） |
| `events` | 事件流：报警/解除/上线/离线/下发/回执 |

想从零开始：停掉服务后删掉 `data\iot.db*` 三个文件，下次启动自动重建。
保留天数在 `config.json` 的 `limits.keep_days` 里改。

---

## 七、config.json 速查

| 键 | 说明 |
|---|---|
| `gateway_id` | 网关编号，必须与 ESP32 的 `APP_GATEWAY_ID` 一致（默认 `gw01`） |
| `web.access_password` | 网页访问口令 |
| `mqtt.backend_user/password` | 后端账号（只在本机用） |
| `mqtt.device_user/password` | 设备账号，必须与 ESP32 的 `APP_MQTT_USERNAME/PASSWORD` 一致 |
| `cloudflare.tunnel_token` | 隧道 token（手机外网访问用） |
| `devices.labels` | 节点标签池（按接入顺序自动分配 A/B/C…） |
| `cmd_presets` | 网页上的快捷指令按钮 |
| `limits.tele_store_sec` | 曲线入库间隔（秒，默认 2 = 与节点上报同节奏） |
| `limits.tele_keep_days` | 曲线原始点保留天数（默认 7；事件仍按 `keep_days` 保留 30 天） |
| `limits.series_max_points` | 曲线接口单次最多返回多少个点（超出自动抽稀，默认 1500） |
| `limits.offline_timeout_sec` | 多久没数据算离线（秒） |

**注意**：改了 `mqtt` 里的账号密码后，要重新跑一次 `setup.ps1`（重新生成 Mosquitto 的密码与 ACL），
并同步改 `gateway_esp32s3\main\app_config.h`。

---

## 八、故障排查

| 现象 | 处理 |
|---|---|
| `start.ps1` 提示缺少 runtime.json | 先跑一次 `setup.ps1` |
| Mosquitto 起不来 | 看 `<运行目录>\logs\mosquitto.log`；确认 1883 没被别的程序占用 |
| 网页打不开但你人在家里 | 先试 `http://127.0.0.1:3000`；能开说明是隧道的问题 |
| 网关一直离线 | ESP32 没连上 MQTT：看它的串口、核对 `APP_MQTT_URI` 的 IP、检查防火墙 |
| 节点一直离线 | 网关没连上该节点，看网关串口状态表；或节点掉电 |
| 网页数据卡住不动 | `Get-Process mosquitto,python,cloudflared` 看是否被"加速软件"杀掉了；重启 `start.ps1` |
| 手机打不开公网地址 | 域名状态是否 Active、Public Hostname 是否配好、笔记本是否断网 |
