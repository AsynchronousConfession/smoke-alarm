#!/usr/bin/env bash
# =============================================================================
#  家庭室内多节点烟雾报警系统 —— 云服务器一键部署脚本（Linux）
#
#  用法（在服务器上，root 权限）：
#     mkdir -p /root/smoke_deploy
#     tar -xzf /root/smoke_server_deploy.tar.gz -C /root/smoke_deploy
#     sudo bash /root/smoke_deploy/install_on_server.sh
#
#  可选参数（用环境变量传）：
#     APP_DIR=/opt/smoke-alarm   安装目录（默认 /opt/smoke-alarm）
#     WEB_PORT=3000              网页端口（默认取 config.json 里的值）
#
#  卸载（保留数据库）：
#     sudo bash install_on_server.sh --uninstall
#
#  说明：脚本是幂等的，重复执行会更新代码并重启服务。
# =============================================================================
set -euo pipefail

APP_DIR="${APP_DIR:-/opt/smoke-alarm}"
SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BACKEND_USER="${BACKEND_USER:-smoke}"

log()  { printf '\033[36m[%s]\033[0m %s\n' "$(date +%H:%M:%S)" "$*"; }
warn() { printf '\033[33m[警告]\033[0m %s\n' "$*"; }
die()  { printf '\033[31m[错误]\033[0m %s\n' "$*" >&2; exit 1; }

[[ $EUID -eq 0 ]] || die "请用 root 运行：sudo bash install_on_server.sh"

# ---------------------------------------------------------------- 卸载 --------
if [[ "${1:-}" == "--uninstall" ]]; then
    log "停止并删除服务（数据库保留在 $APP_DIR/data）"
    systemctl disable --now smoke-backend smoke-mosquitto 2>/dev/null || true
    rm -f /etc/systemd/system/smoke-backend.service /etc/systemd/system/smoke-mosquitto.service
    systemctl daemon-reload
    log "已卸载。代码和数据库还在 $APP_DIR，确认没用后可以手动 rm -rf $APP_DIR"
    exit 0
fi

[[ -f "$SRC_DIR/config.json" && -f "$SRC_DIR/backend/app.py" ]] || \
    die "当前目录不像部署包：缺少 config.json / backend/app.py。请先解压部署包，再在解压目录里运行本脚本。"

# ------------------------------------------------------------ 1. 识别系统 ----
if command -v apt-get >/dev/null 2>&1; then PKG=apt
elif command -v dnf >/dev/null 2>&1; then PKG=dnf
elif command -v yum >/dev/null 2>&1; then PKG=yum
else die "不支持的系统：没有找到 apt / dnf / yum"
fi
log "系统包管理器: $PKG"

# ------------------------------------------------------------ 2. 装依赖 ------
log "安装依赖（mosquitto / python3 / venv / pip）…"
case "$PKG" in
    apt)
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -y >/dev/null
        apt-get install -y mosquitto mosquitto-clients python3 python3-venv python3-pip ca-certificates >/dev/null
        ;;
    dnf|yum)
        $PKG install -y python3 python3-pip ca-certificates >/dev/null
        if ! $PKG install -y mosquitto >/dev/null 2>&1; then
            log "默认源里没有 mosquitto，尝试启用 EPEL…"
            $PKG install -y epel-release >/dev/null 2>&1 || true
            $PKG install -y mosquitto >/dev/null || \
                die "装不上 mosquitto。请手动安装后重跑（EPEL 路线：dnf install -y epel-release && dnf install -y mosquitto）"
        fi
        ;;
esac

MOSQ_BIN="$(command -v mosquitto)" || die "找不到 mosquitto 可执行文件"
MOSQ_PW_BIN="$(command -v mosquitto_passwd)" || die "找不到 mosquitto_passwd"
log "mosquitto: $MOSQ_BIN"

# ------------------------------------------------------- 3. 拷贝代码 ---------
log "安装代码到 $APP_DIR"
mkdir -p "$APP_DIR"
( cd "$SRC_DIR" && tar --exclude=.venv --exclude=__pycache__ --exclude=logs \
      --exclude='data/*.db*' -cf - . ) | ( cd "$APP_DIR" && tar -xf - )
mkdir -p "$APP_DIR/data"

# ------------------------------------------------- 4. Python 虚拟环境 --------
if [[ ! -x "$APP_DIR/.venv/bin/python" ]]; then
    log "创建 Python 虚拟环境"
    python3 -m venv "$APP_DIR/.venv" || die "venv 创建失败（Debian/Ubuntu 请先 apt install python3-venv python3-full）"
fi
PY="$APP_DIR/.venv/bin/python"
PIP="$APP_DIR/.venv/bin/pip"
"$PIP" install -q --upgrade pip >/dev/null 2>&1 || true
log "安装 Python 依赖 paho-mqtt"
if ! "$PIP" install -q -r "$APP_DIR/backend/requirements.txt" -i https://pypi.tuna.tsinghua.edu.cn/simple; then
    warn "清华镜像装不上，改用包内自带的 wheel（离线安装）"
    "$PIP" install -q --no-index --find-links "$APP_DIR/tools" -r "$APP_DIR/backend/requirements.txt" || \
        die "paho-mqtt 安装失败，请检查服务器网络"
fi
"$PY" -c "import paho.mqtt; print('  paho-mqtt', paho.mqtt.__version__)"

# -------------------------------------------------- 5. 读取配置 --------------
eval "$(python3 - "$APP_DIR/config.json" <<'PY'
import json, shlex, sys
c = json.load(open(sys.argv[1], encoding="utf-8"))
m = c["mqtt"]
pairs = {
    "CFG_GW":        c["gateway_id"],
    "CFG_MQTT_PORT": m["port"],
    "CFG_BUSER":     m["backend_user"],
    "CFG_BPASS":     m["backend_password"],
    "CFG_DUSER":     m["device_user"],
    "CFG_DPASS":     m["device_password"],
    "CFG_WEBPORT":   c["web"]["port"],
    "CFG_WEBPW":     c["web"]["access_password"],
}
for k, v in pairs.items():
    print("%s=%s" % (k, shlex.quote(str(v))))
PY
)"
[[ -n "${WEB_PORT:-}" ]] && CFG_WEBPORT="$WEB_PORT"
log "网关编号=$CFG_GW  MQTT端口=$CFG_MQTT_PORT  网页端口=$CFG_WEBPORT"

# -------------------------------------------------- 6. Mosquitto -------------
# Debian 系：全部放在 $APP_DIR/mosquitto（简单直观）
# RHEL 系：用发行版标准路径（SELinux 对这几种路径有默认标签，不容易被拦）
if [[ "$PKG" == "apt" ]]; then
    MOSQ_DIR="$APP_DIR/mosquitto"
    MOSQ_CONF="$MOSQ_DIR/mosquitto.conf"
    MOSQ_PW="$MOSQ_DIR/passwd"
    MOSQ_ACL="$MOSQ_DIR/acl"
    MOSQ_DATA="$MOSQ_DIR/data"
    MOSQ_LOG="$MOSQ_DIR/logs"
else
    MOSQ_CONF=/etc/mosquitto/mosquitto.conf
    MOSQ_PW=/etc/mosquitto/passwd
    MOSQ_ACL=/etc/mosquitto/acl
    MOSQ_DATA=/var/lib/mosquitto
    MOSQ_LOG=/var/log/mosquitto
fi
mkdir -p "$MOSQ_DATA" "$MOSQ_LOG" "$(dirname "$MOSQ_CONF")"

log "生成 Mosquitto 密码文件与 ACL"
rm -f "$MOSQ_PW"
"$MOSQ_PW_BIN" -c -b "$MOSQ_PW" "$CFG_BUSER" "$CFG_BPASS"
"$MOSQ_PW_BIN"    -b "$MOSQ_PW" "$CFG_DUSER" "$CFG_DPASS"

cat > "$MOSQ_ACL" <<EOF
# 后端：可以订阅全部主题，并且只能向 cmd 主题下发
user $CFG_BUSER
topic read home/#
topic write home/+/cmd/+

# 网关设备：只能上报自己网关的数据，并且只能读命令主题
user $CFG_DUSER
topic write home/+/tele/+
topic write home/+/event/+
topic write home/+/state/+
topic write home/+/ack/+
topic read  home/+/cmd/+
EOF

cat > "$MOSQ_CONF" <<EOF
# 本文件由 install_on_server.sh 自动生成
listener $CFG_MQTT_PORT 0.0.0.0
allow_anonymous false
password_file $MOSQ_PW
acl_file $MOSQ_ACL
persistence true
persistence_location $MOSQ_DATA/
log_dest file $MOSQ_LOG/mosquitto.log
log_type error
log_type warning
log_type notice
connection_messages true
EOF
# 注意：不要写 "max_keepalive 0"。
# Ubuntu 22.04 自带 mosquitto 2.0.x，认为 0 是非法值，会直接启动失败：
#     Error: Invalid max_keepalive value (0)
# 不写这一行 = 不限制客户端 keepalive，正是我们要的效果。

# -------------------------------------------------- 7. 系统用户与权限 --------
if ! id -u "$BACKEND_USER" >/dev/null 2>&1; then
    log "创建系统用户 $BACKEND_USER"
    useradd --system --create-home --shell /sbin/nologin "$BACKEND_USER" 2>/dev/null || \
        useradd -r -s /sbin/nologin "$BACKEND_USER" 2>/dev/null || true
fi
chown -R "$BACKEND_USER:$BACKEND_USER" "$APP_DIR"

if id -u mosquitto >/dev/null 2>&1; then
    MOSQ_USER=mosquitto
else
    MOSQ_USER="$BACKEND_USER"
fi
chown -R "$MOSQ_USER:$MOSQ_USER" "$MOSQ_DATA" "$MOSQ_LOG" 2>/dev/null || true
chown "$MOSQ_USER" "$MOSQ_PW" "$MOSQ_ACL" "$MOSQ_CONF" 2>/dev/null || true
chmod 640 "$MOSQ_PW"

# -------------------------------------------------- 8. systemd 服务 ----------
log "写入 systemd 服务"
cat > /etc/systemd/system/smoke-mosquitto.service <<EOF
[Unit]
Description=Smoke Alarm MQTT Broker (mosquitto)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=$MOSQ_USER
ExecStart=$MOSQ_BIN -c $MOSQ_CONF
Restart=always
RestartSec=3
LimitNOFILE=8192

[Install]
WantedBy=multi-user.target
EOF

cat > /etc/systemd/system/smoke-backend.service <<EOF
[Unit]
Description=Smoke Alarm Web Backend (app.py)
After=network-online.target smoke-mosquitto.service
Wants=network-online.target

[Service]
Type=simple
User=$BACKEND_USER
WorkingDirectory=$APP_DIR/backend
ExecStart=$PY -u $APP_DIR/backend/app.py
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
EOF

# 发行版自带的 mosquitto 服务会占用 1883，先关掉
systemctl disable --now mosquitto 2>/dev/null || true
systemctl stop mosquitto 2>/dev/null || true

# Ubuntu 的 mosquitto 可能带 AppArmor 限制，会拦住 /opt 下的配置文件
if [[ -e /etc/apparmor.d/usr.sbin.mosquitto ]]; then
    warn "检测到 mosquitto 的 AppArmor 限制，正在禁用它（否则读不到 $MOSQ_CONF）"
    mkdir -p /etc/apparmor.d/disable
    ln -sf /etc/apparmor.d/usr.sbin.mosquitto /etc/apparmor.d/disable/ 2>/dev/null || true
    apparmor_parser -R /etc/apparmor.d/usr.sbin.mosquitto 2>/dev/null || true
fi

# 端口还被占用的话，干掉"用发行版配置"的那个 mosquitto（不会误伤我们的服务）
# 注意：重新部署时我们自己的服务本来就在监听这个端口，所以先判断服务是否已经在跑
if ! systemctl is-active --quiet smoke-mosquitto && ss -lnt 2>/dev/null | grep -q ":${CFG_MQTT_PORT} "; then
    warn "端口 ${CFG_MQTT_PORT} 已被占用，尝试释放（很可能是系统自带的 mosquitto）"
    pkill -f "mosquitto -c /etc/mosquitto" 2>/dev/null || true
    sleep 1
fi

systemctl daemon-reload
systemctl enable --now smoke-mosquitto smoke-backend

# -------------------------------------------------- 9. 本机防火墙 -------------
if systemctl is-active --quiet firewalld; then
    firewall-cmd --permanent --add-port=${CFG_MQTT_PORT}/tcp >/dev/null 2>&1 || true
    firewall-cmd --permanent --add-port=${CFG_WEBPORT}/tcp  >/dev/null 2>&1 || true
    firewall-cmd --reload >/dev/null 2>&1 || true
    log "已在本机 firewalld 放行 ${CFG_MQTT_PORT} / ${CFG_WEBPORT}"
elif command -v ufw >/dev/null 2>&1 && ufw status 2>/dev/null | grep -q "Status: active"; then
    ufw allow ${CFG_MQTT_PORT}/tcp >/dev/null 2>&1 || true
    ufw allow ${CFG_WEBPORT}/tcp  >/dev/null 2>&1 || true
    log "已在本机 ufw 放行 ${CFG_MQTT_PORT} / ${CFG_WEBPORT}"
fi

sleep 2

# 起不来的话直接把日志打出来，省得再问"为什么失败"
for svc in smoke-mosquitto smoke-backend; do
    if ! systemctl is-active --quiet "$svc"; then
        warn "==================================================="
        warn "$svc 没有起来，最近 25 行日志："
        journalctl -u "$svc" -n 25 --no-pager 2>/dev/null | sed 's/^/    /' || true
        warn "==================================================="
    fi
done

# -------------------------------------------------- 10. 结果 ------------------
IP="$(curl -s --max-time 5 https://ipinfo.io/ip 2>/dev/null || hostname -I | awk '{print $1}')"
echo
echo "==================== 部署结果 ===================="
systemctl is-active --quiet smoke-mosquitto && echo "  MQTT 服务   : 运行中 (端口 ${CFG_MQTT_PORT})" || warn "  MQTT 服务没起来，看：journalctl -u smoke-mosquitto -n 50"
systemctl is-active --quiet smoke-backend   && echo "  网页后端    : 运行中 (端口 ${CFG_WEBPORT})" || warn "  网页后端没起来，看：journalctl -u smoke-backend -n 50"
echo "  监听端口    :"
ss -lntp 2>/dev/null | grep -E ":(${CFG_MQTT_PORT}|${CFG_WEBPORT})\b" | sed 's/^/    /' || true
echo
echo "  网页访问    : http://${IP}:${CFG_WEBPORT}     （口令：${CFG_WEBPW}）"
echo "  ESP32 要改  : gateway_esp32s3/main/app_config.h 里的"
echo "                #define APP_MQTT_URI  \"mqtt://${IP}:${CFG_MQTT_PORT}\""
echo
echo "  还要做两件事："
echo "    1) 阿里云控制台 → 安全组 → 入方向放行 TCP ${CFG_MQTT_PORT} 和 ${CFG_WEBPORT}"
echo "    2) 重新编译烧写 ESP32 网关固件（MQTT 地址换成上面的）"
echo
echo "  常用命令："
echo "    systemctl status smoke-mosquitto smoke-backend"
echo "    journalctl -u smoke-backend -f          # 看后端日志"
echo "    tail -f $MOSQ_LOG/mosquitto.log         # 看 MQTT 日志"
echo "    sudo bash $SRC_DIR/install_on_server.sh --uninstall   # 卸载"
echo "=================================================="
