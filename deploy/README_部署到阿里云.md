# 部署到阿里云服务器（完整步骤）

> 目标：把 **Mosquitto + 后端 + 网页** 从你的笔记本搬到阿里云，
> 这样你的 ESP32 网关只要连着家里的 WiFi 就能一直上报，不再依赖笔记本开机。
>
> 节点和网关固件**只改一行**：MQTT 地址换成云服务器的公网 IP。

---

## 0. 你问的"要给我哪些信息"

发我下面这些就行（都是**非敏感**信息）：

| 需要的信息 | 怎么拿到 | 我拿它干什么 |
|---|---|---|
| **公网 IP** | 阿里云控制台 → 实例列表（"IP 地址"那一列的**公网** IP） | 写进部署命令、ESP32 的 MQTT 地址 |
| **操作系统和版本** | 服务器上执行 `cat /etc/os-release` | 决定用 apt 还是 dnf/yum 分支 |
| **CPU 架构** | 服务器上执行 `uname -m` | 需要时选对 cloudflared 的安装包（x86_64 / aarch64） |
| **内存 / 磁盘** | `free -h` 和 `df -h` | 确认够用（本系统只要 ~300MB 内存） |
| **安全组想放行的端口** | 你自己决定（建议 1883 + 3000） | 我核对阿里云控制台该加哪几条规则 |
| **有没有域名 / 是否备案**（可选） | — | 决定用"IP 直连"还是"域名+HTTPS 隧道" |

### ⚠️ 千万不要发给任何人（包括我）

- 服务器 **root 密码**、SSH **私钥**（`id_rsa`）
- 阿里云 **AccessKey ID / Secret**
- `config.json` 里现有的密码、Cloudflare token

这套方案**不需要**我登录你的服务器：我负责把脚本和配置写好，你在服务器上粘几行命令就能跑起来。

---

## 1. 部署前必做：先改口令（重要）

服务器放到公网后，1883 会被全世界的扫描器敲门，**必须**换成强口令。改 `server\config.json`：

```json
"web":  { "access_password": "……" },
"mqtt": { "backend_password": "……", "device_password": "……" }
```

生成随机口令（在 Windows PowerShell 里）：

```powershell
-join ((48..57)+(65..90)+(97..122) | Get-Random -Count 16 | % {[char]$_})
```

服务器上也可以：`openssl rand -base64 12`

> 口令改完，**部署脚本会自动把它同步到 Mosquitto 的密码文件和 ACL**；
> 只有 ESP32 那边要跟着改（见第 5 节）。
> 如果你还想让笔记本那套继续能用，记得同步改笔记本上的 `config.json` 并重跑一次 `setup.ps1`。

---

## 2. 在本机打包并上传

```powershell
cd "D:\smoke-alarm\04"
powershell -ExecutionPolicy Bypass -File .\deploy\pack_for_upload.ps1
```

会生成 `deploy\smoke_server_deploy.tar.gz`（约几十 KB，自动排除本机虚拟环境、日志、Windows 专用 exe）。

上传（把 IP 换成你的）：

```powershell
scp .\deploy\smoke_server_deploy.tar.gz root@<公网IP>:/root/
```

> 没有 scp / 不想用命令行：阿里云控制台 → 实例 → **远程连接 → Workbench** →
> 右上角有"文件上传"，把 tar.gz 传上去即可（传到 `/root/`）。

---

## 3. 在服务器上执行部署

用 Workbench（网页终端）或 SSH 登录服务器，粘下面三行：

```bash
mkdir -p /root/smoke_deploy
tar -xzf /root/smoke_server_deploy.tar.gz -C /root/smoke_deploy
sudo bash /root/smoke_deploy/install_on_server.sh
```

脚本会自动完成：装依赖（mosquitto / python3）→ 建虚拟环境装 paho-mqtt →
生成 Mosquitto 密码与 ACL → 装 systemd 服务并开机自启 →
本机防火墙放行 → 打印结果。

成功时最后会打印：

```
==================== 部署结果 ====================
  MQTT 服务   : 运行中 (端口 1883)
  网页后端    : 运行中 (端口 3000)
  监听端口    :
    LISTEN 0 128 0.0.0.0:1883 ...
    LISTEN 0 128 0.0.0.0:3000 ...

  网页访问    : http://<公网IP>:3000
  ESP32 要改  : gateway_esp32s3/main/app_config.h 里的
                #define APP_MQTT_URI  "mqtt://<公网IP>:1883"
```

> 脚本是**幂等**的：以后更新代码，重新上传 + 重跑一遍就行（数据库不会丢）。
> 卸载：`sudo bash /root/smoke_deploy/install_on_server.sh --uninstall`（数据库保留）。

---

## 4. 阿里云控制台：放行端口（安全组）

**这一步不做，外面就连不上**。控制台路径：

```
云服务器 ECS → 实例 → 点进你的实例 → 安全组 → 配置规则
  → 入方向 → 手动添加：
      协议类型 TCP    端口范围 1883/1883    源 0.0.0.0/0    备注 MQTT
      协议类型 TCP    端口范围 3000/3000    源 0.0.0.0/0    备注 网页
  → 保存（立刻生效，不用重启）
```

> - **1883** 是给 ESP32 网关上报/收指令用的，必须对公网开放（靠账号密码 + ACL 保护）。
> - **3000** 是给手机/电脑看网页用的。只在家里看的话，可以把它限制成你的家庭宽带 IP。
> - 阿里云免费实例的带宽通常只有 **1~3 Mbps**：网页首次加载约 1.1 MB（含图表库），
>   大概 3~10 秒；之后走缓存就很快，MQTT 数据量极小（几 KB/s），完全够用。

---

## 5. 改 ESP32 网关并重新烧写

只改一行 —— `gateway_esp32s3\main\app_config.h`：

```c
#define APP_MQTT_URI   "mqtt://<公网IP>:1883"     /* 换成云服务器公网 IP */
/* 账号密码保持与 server\config.json 一致（若你改了口令，这里也要改） */
#define APP_MQTT_USERNAME  "esp32gw"
#define APP_MQTT_PASSWORD  "……"
```

然后：

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\build_gateway.ps1 -Port COM6 -Flash -Monitor
```

串口看到这两行就成功了：

```
I (…) NET: WiFi 已连接, IP: 192.168.x.x
I (…) NET: MQTT 已连接: mqtt://<公网IP>:1883
```

> 节点（CH573F）**完全不用动**，它们只跟网关说话。
> 笔记本那套可以停掉：`server\start.ps1 -Stop`（两套同时跑也不会冲突，
> 但 ESP32 只会连到你配置的那一个）。

---

## 6. 验证清单

- [ ] 服务器上 `systemctl status smoke-mosquitto smoke-backend` 都是 `active (running)`
- [ ] 服务器上 `ss -lntp | grep -E '1883|3000'` 能看到两个 0.0.0.0 监听
- [ ] 手机用**流量**打开 `http://<公网IP>:3000`，能出现登录页（这一步验证安全组）
- [ ] 输入口令后能看到大屏，节点卡片显示在线
- [ ] 打火机测试：网页变红 + 曲线掉下去 + 事件流出现"报警"
- [ ] 网页点"消音 60s"，节点蜂鸣器停，事件流出现回执
- [ ] 服务器重启后（`sudo reboot`）两个服务能自动起来

---

## 7. 可选：用域名 + HTTPS（不用备案）

想给手机一个"正经网址"、又不想买备案，可以继续用 **Cloudflare 隧道**，
只是把 cloudflared 从笔记本搬到云服务器（Linux 版）：

```bash
# 1) 装 cloudflared（x86_64；ARM 机器把 amd64 换成 arm64）
curl -L -o /usr/local/bin/cloudflared \
  https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-linux-amd64
chmod +x /usr/local/bin/cloudflared

# 2) 用你现有的隧道 token 起服务（token 在 server\config.json 里）
cloudflared service install <把你的 tunnel_token 粘这里>
systemctl enable --now cloudflared
```

之后手机访问 `https://iot.你的域名`，不用开 3000 端口（1883 仍然要给 ESP32 用）。

> **备案说明**：用 `IP:端口` 访问国内服务器**不需要**备案；
> 用**域名**指向国内服务器且走 **80/443** 才需要备案。
> 走 Cloudflare 隧道时，公网流量是从 Cloudflare 回源到你的服务器，域名解析在境外，也不需要备案。

---

## 8. 日常运维

| 想干什么 | 命令 |
|---|---|
| 看后端日志 | `journalctl -u smoke-backend -f` |
| 看 MQTT 日志 | `tail -f /opt/smoke-alarm/mosquitto/logs/mosquitto.log`（RHEL 系在 `/var/log/mosquitto/`） |
| 重启服务 | `sudo systemctl restart smoke-backend smoke-mosquitto` |
| 更新代码 | 本机重新打包 → scp 上传 → 重跑 `install_on_server.sh` |
| 备份数据库 | `scp root@<IP>:/opt/smoke-alarm/data/iot.db ./iot_备份.db` |
| 改口令 | 改本机 `config.json` → 重新打包上传 → 重跑脚本（会自动重建密码文件） |
| 看端口占用 | `sudo ss -lntp \| grep -E '1883\|3000'` |
| 卸载 | `sudo bash /root/smoke_deploy/install_on_server.sh --uninstall` |

---

## 9. 常见问题

| 现象 | 原因 / 处理 |
|---|---|
| 手机打不开网页 | 九成是**安全组**没放行 3000；其次看本机防火墙：`firewall-cmd --list-ports` / `ufw status` |
| ESP32 一直 `MQTT 断开` | ①`APP_MQTT_URI` 的 IP/端口不对；②安全组没放行 1883；③账号密码与 `config.json` 不一致；④服务器没跑 `smoke-mosquitto` |
| 网页能开但节点离线 | 看后端日志有没有收到 MQTT：`journalctl -u smoke-backend -f`；再核对 ESP32 串口是否 `MQTT 已连接` |
| mosquitto 起不来 | `journalctl -u smoke-mosquitto -n 50`；常见是端口被系统自带 mosquitto 占用（脚本已自动关掉它）或 SELinux（看 `getenforce`，临时 `setenforce 0` 验证） |
| pip 装依赖很慢 | 脚本默认用清华镜像；也可以直接把本机 `tools\paho_mqtt-*.whl` 一起上传，脚本会自动用它离线安装 |
| 云上打开是空数据 | 正常：云服务器是**从零开始**记录，本机那 7 天历史不会一起搬过去 |
| 想限制访问来源 | 安全组里把 3000 的"源"改成你家宽带的公网 IP；或者只走隧道不开 3000 |

---

## 10. 这套部署在服务器上到底装了什么

| 组件 | 位置 | 说明 |
|---|---|---|
| 代码 | `/opt/smoke-alarm/` | 后端 + 网页 + 配置 |
| 数据库 | `/opt/smoke-alarm/data/iot.db` | SQLite，启动时自动建表 |
| Mosquitto | 配置/密码/日志见脚本输出 | 独立 systemd 服务 `smoke-mosquitto` |
| 后端 | `smoke-backend.service` | 端口 3000，`Restart=always` |
| 运行用户 | `smoke` | 后端以普通系统用户运行，不用 root |

两个服务都设了开机自启，服务器重启后会自动恢复。

---

## 附录：本次部署的实际情况（2026-10-03 记录）

| 项目 | 值 |
|---|---|
| 云服务器公网 IP | **192.0.2.10** |
| 内网 IP / 主机名 | 172.17.137.89 / iZ7xv1ep7sy6eytxwtw2yvZ |
| 操作系统 | **Ubuntu 22.04.5 LTS**（x86_64，内核 5.15） |
| 配置 | 1.7 GB 内存 / 40 GB 磁盘（可用 34 GB，够用得很） |
| 域名 | `smoke.example.com`（**未备案**） |
| 访问方式 | 手机/电脑：`https://smoke.example.com`（Cloudflare 隧道，不需备案）<br>调试备用：`http://192.0.2.10:3000`（IP + 端口，不需备案） |
| ESP32 上报地址 | `mqtt://192.0.2.10:1883`（改 `app_config.h` 的 `APP_MQTT_URI`） |

**因为用的是 Ubuntu，安装脚本会走 apt 分支**，实际等价于：

```bash
# 1) 本机打包 + 上传
powershell -ExecutionPolicy Bypass -File .\deploy\pack_for_upload.ps1
scp .\deploy\smoke_server_deploy.tar.gz root@192.0.2.10:/root/

# 2) 服务器上部署
mkdir -p /root/smoke_deploy
tar -xzf /root/smoke_server_deploy.tar.gz -C /root/smoke_deploy
sudo bash /root/smoke_deploy/install_on_server.sh
```

**注意（未备案域名）**：

- ❌ 不要把这个域名解析到 `192.0.2.10` 后走 80/443 —— 阿里云对未备案域名会拦截 80/443；
- ✅ 用 `http://192.0.2.10:3000` 或 Cloudflare 隧道（`https://smoke.example.com`）都合规、都能用；
- ⚠️ 同一套隧道 token **只能在一台机器上跑**：云服务器启用隧道后，笔记本上要执行
  `server\start.ps1 -Stop`，否则域名会在这两台之间轮询，出现"一会儿正常一会儿空白"。

### 部署进度（2026-10-03 实际执行记录）

| 步骤 | 状态 | 说明 |
|---|---|---|
| 上传 + 安装脚本 | ✅ 完成 | 后端 3000 端口已监听 |
| Mosquitto 启动 | ✅ 已修复 | Ubuntu 的 mosquitto 2.0 **不接受 `max_keepalive 0`**，删掉该行后正常（脚本已同步修正） |
| MQTT 端到端自检 | ✅ 通过 | 设备账号发遥测 → 后端入库；ACL 越权订阅被正确拒绝（见 `server_selftest.sh`） |
| Cloudflare 隧道 | ✅ 完成 | 在云服务器上装好并注册成功；从服务器侧访问 `https://smoke.example.com` 返回 **200** |
| 笔记本隧道 | ✅ 已停止 | 避免同一 token 两地争抢 |
| ESP32 网关固件 | ✅ 已烧写 | `APP_MQTT_URI` 已改为 `mqtt://192.0.2.10:1883`，串口确认已连上节点（N01/MQ-2 正常读数据） |
| **阿里云安全组** | ⏳ **待你在控制台放行** | 需要放行 **TCP 1883**（ESP32 用）；3000 可选（走隧道就不需要） |

**只剩最后一步**：安全组放行 1883 之后，ESP32 会在几秒内自动连上云端，
网页/手机立刻能看到 N01 的实时数据（固件已内置断线自动重连，不需要再动硬件）。

> 现象提醒：**用笔记本打开 `https://smoke.example.com` 会被重置**，
> 这是本地宽带的运营商对该域名做了 SNI 拦截（同一网络下 `www.cloudflare.com`、百度都正常，
> 而云服务器访问该域名返回 200）。换成手机流量访问即可，与服务器配置无关。
