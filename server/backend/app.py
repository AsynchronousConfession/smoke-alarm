#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
家庭室内多节点烟雾报警系统 —— PC 端后端

职责:
  1) 订阅本地 Mosquitto 的 MQTT 主题, 把 ESP32-S3 网关上报的数据写入 SQLite
  2) 通过 SSE 把新数据实时推给浏览器
  3) 接收网页下发的控制指令(消音/自检/复位/文本), 用 MQTT 发给网关
  4) 提供 REST 接口与静态网页

MQTT 主题约定（与 ESP32 侧 main/wifi_mqtt.c 保持一致）:
    home/<网关ID>/tele/<MAC>     遥测数据        (设备 -> 云)
    home/<网关ID>/event/<MAC>    报警/解除事件   (设备 -> 云)
    home/<网关ID>/state/<MAC>    节点在线状态     (设备 -> 云, retain)
    home/<网关ID>/state/gateway  网关自身状态     (设备 -> 云, retain)
    home/<网关ID>/ack/<MAC>      指令回执        (设备 -> 云)
    home/<网关ID>/cmd/<MAC>      控制指令(纯文本) (云 -> 设备)

只用标准库 + 纯 Python 的 paho-mqtt（见 requirements.txt），
不引入 FastAPI/pydantic，避免在老机器上现场编译 Rust。
"""

import json
import math
import mimetypes
import os
import queue
import socket
import sqlite3
import threading
import time
import gzip
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs, unquote

import paho.mqtt.client as mqtt

BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONFIG_PATH = os.path.join(BASE_DIR, "config.json")
DATA_DIR = os.path.join(BASE_DIR, "data")
DB_PATH = os.path.join(DATA_DIR, "iot.db")
WEB_DIR = os.path.join(BASE_DIR, "web")

with open(CONFIG_PATH, "r", encoding="utf-8") as fp:
    CFG = json.load(fp)

GW_ID = CFG["gateway_id"]
TOPIC_BASE = "home/%s" % GW_ID
LABELS = CFG.get("devices", {}).get("labels", ["A", "B", "C", "D", "E", "F"])
WEB_PASSWORD = CFG["web"]["access_password"]
HTTP_HOST = CFG["web"]["host"]
HTTP_PORT = int(CFG["web"]["port"])
MAX_CMD_LEN = int(CFG.get("limits", {}).get("max_cmd_len", 20))
KEEP_DAYS = int(CFG.get("limits", {}).get("keep_days", 30))
# 曲线点存得密（默认 2 秒一个），单独设一个更短的保留期，避免数据库无限长大
TELE_KEEP_DAYS = int(CFG.get("limits", {}).get("tele_keep_days", 7))

# 遥测入库节流：一个节点最快 N 秒写一条曲线点（报警跳变时会立刻补一条）
TELE_STORE_SEC = int(CFG.get("limits", {}).get("tele_store_sec", 2))
# 多久没收到数据就算离线
OFFLINE_TIMEOUT = int(CFG.get("limits", {}).get("offline_timeout_sec", 90))
# 曲线接口单次最多返回多少个点（超过就等间隔抽稀，保证长时段也画得动）
SERIES_MAX_POINTS = int(CFG.get("limits", {}).get("series_max_points", 1500))

# 网页上的快捷指令（点一下就把 text 发下去）
CMD_PRESETS = CFG.get("cmd_presets", [
    {"label": "消音 60s", "text": "M"},
    {"label": "自检", "text": "T"},
    {"label": "复位", "text": "R"},
])

DB_LOCK = threading.Lock()
DB = None

# gzip 压缩缓存：静态文件压缩一次就缓存起来（键里带 mtime，文件改了会自动重压）
GZIP_LOCK = threading.Lock()
GZIP_CACHE = {}
GZIP_MIN_BYTES = 256


def gz(data):
    """gzip 压缩（mtime=0 保证结果稳定，便于缓存）"""
    return gzip.compress(data, 6, mtime=0)


def log(*a):
    print(time.strftime("[%H:%M:%S]"), *a, flush=True)


# ---------------------------------------------------------------------------
# 数据库
# ---------------------------------------------------------------------------
def db_init():
    global DB
    os.makedirs(DATA_DIR, exist_ok=True)
    DB = sqlite3.connect(DB_PATH, check_same_thread=False)
    DB.row_factory = sqlite3.Row
    with DB_LOCK:
        DB.execute("PRAGMA journal_mode=WAL")

        # 节点台账
        DB.execute("""CREATE TABLE IF NOT EXISTS devices (
                        mac          TEXT PRIMARY KEY,
                        label        TEXT,
                        tag          TEXT,
                        model        TEXT,
                        fw           TEXT,
                        first_seen   INTEGER,
                        last_seen    INTEGER,
                        online       INTEGER DEFAULT 0,
                        rssi         INTEGER,
                        dpct         REAL,
                        adc          INTEGER,
                        ao_mv        INTEGER,
                        rs_ohm       INTEGER,
                        alarm        INTEGER DEFAULT 0,
                        warmup       INTEGER DEFAULT 0,
                        muted        INTEGER DEFAULT 0,
                        tele_count   INTEGER DEFAULT 0,
                        alarm_count  INTEGER DEFAULT 0,
                        last_alarm_ts INTEGER)""")

        # 曲线数据（节流存储）
        DB.execute("""CREATE TABLE IF NOT EXISTS telemetry (
                        id     INTEGER PRIMARY KEY AUTOINCREMENT,
                        ts     INTEGER NOT NULL,
                        mac    TEXT    NOT NULL,
                        dpct   REAL,
                        adc    INTEGER,
                        rs_ohm INTEGER,
                        alarm  INTEGER)""")

        # 事件流：报警/解除/上线下线/下发/回执
        DB.execute("""CREATE TABLE IF NOT EXISTS events (
                        id     INTEGER PRIMARY KEY AUTOINCREMENT,
                        ts     INTEGER NOT NULL,
                        mac    TEXT    NOT NULL,
                        kind   TEXT    NOT NULL,
                        text   TEXT,
                        dpct   REAL,
                        rssi   INTEGER)""")

        DB.execute("CREATE INDEX IF NOT EXISTS idx_tele_ts  ON telemetry(ts DESC)")
        DB.execute("CREATE INDEX IF NOT EXISTS idx_tele_mac ON telemetry(mac, ts DESC)")
        DB.execute("CREATE INDEX IF NOT EXISTS idx_ev_ts    ON events(ts DESC)")
        DB.commit()
    db_purge()


def db_purge():
    now = int(time.time())
    with DB_LOCK:
        # 曲线点存得密，保留期短一些；事件（报警记录）保留久一些
        DB.execute("DELETE FROM telemetry WHERE ts < ?", (now - TELE_KEEP_DAYS * 86400,))
        DB.execute("DELETE FROM events WHERE ts < ?", (now - KEEP_DAYS * 86400,))
        DB.commit()


def db_execute(sql, args=()):
    with DB_LOCK:
        DB.execute(sql, args)
        DB.commit()


def db_query(sql, args=()):
    with DB_LOCK:
        return [dict(r) for r in DB.execute(sql, args).fetchall()]


def ensure_device(mac):
    """首次见到的 MAC 自动分配 A/B/C… 标签, 之后固定不变。"""
    rows = db_query("SELECT * FROM devices WHERE mac = ?", (mac,))
    if rows:
        return rows[0]
    used = {r["label"] for r in db_query("SELECT label FROM devices")}
    label = "?"
    for cand in LABELS:
        if cand not in used:
            label = cand
            break
    now = int(time.time())
    db_execute("""INSERT OR IGNORE INTO devices(mac,label,first_seen,last_seen)
                  VALUES(?,?,?,?)""", (mac, label, now, now))
    return db_query("SELECT * FROM devices WHERE mac = ?", (mac,))[0]


def add_event(ts, mac, kind, text=None, dpct=None, rssi=None):
    db_execute("INSERT INTO events(ts,mac,kind,text,dpct,rssi) VALUES(?,?,?,?,?,?)",
               (ts, mac, kind, text, dpct, rssi))


# ---------------------------------------------------------------------------
# SSE 客户端
# ---------------------------------------------------------------------------
SSE_LOCK = threading.Lock()
SSE_CLIENTS = set()


def sse_broadcast(payload):
    msg = json.dumps(payload, ensure_ascii=False)
    with SSE_LOCK:
        clients = list(SSE_CLIENTS)
    for q in clients:
        try:
            q.put_nowait(msg)
        except queue.Full:
            pass


# ---------------------------------------------------------------------------
# MQTT
# ---------------------------------------------------------------------------
GW_STATE = {"online": False, "ip": "", "wifi_rssi": None, "nodes": 0,
            "nodes_max": 0, "fw": "", "ts": 0}
MQTT_CLIENT = None
LAST_STORE = {}          # mac -> 上次写曲线的时间
LAST_ALARM = {}          # mac -> 上次的报警状态（用来抓跳变）


def mac_from_topic(topic, leaf):
    parts = topic.split("/")
    if len(parts) == 4 and parts[2] == leaf:
        return parts[3].upper()
    return None


def on_connect(client, userdata, flags, reason_code, properties=None):
    log("[mqtt] connected:", reason_code)
    client.subscribe(TOPIC_BASE + "/tele/+", qos=1)
    client.subscribe(TOPIC_BASE + "/event/+", qos=1)
    client.subscribe(TOPIC_BASE + "/state/+", qos=1)
    client.subscribe(TOPIC_BASE + "/ack/+", qos=1)


def handle_telemetry(mac, raw):
    p = json.loads(raw)
    ts = int(p.get("ts") or time.time())
    dpct = p.get("dpct")
    dpct = float(dpct) if dpct is not None else None
    adc = p.get("adc")
    rs = p.get("rs")
    alarm = 1 if p.get("alarm") else 0

    dev = ensure_device(mac)
    db_execute("""UPDATE devices SET last_seen=?, online=1, rssi=?, dpct=?, adc=?,
                  ao_mv=?, rs_ohm=?, alarm=?, warmup=?, muted=?,
                  tele_count = tele_count + 1 WHERE mac = ?""",
               (int(time.time()), p.get("rssi"), dpct, adc, p.get("ao"), rs,
                alarm, 1 if p.get("warmup") else 0, 1 if p.get("muted") else 0, mac))

    # 曲线节流: 平时每 TELE_STORE_SEC 一条; 报警状态跳变时立刻补一条
    prev_alarm = LAST_ALARM.get(mac)
    if (ts - LAST_STORE.get(mac, 0) >= TELE_STORE_SEC) or (prev_alarm != alarm):
        LAST_STORE[mac] = ts
        db_execute("INSERT INTO telemetry(ts,mac,dpct,adc,rs_ohm,alarm) VALUES(?,?,?,?,?,?)",
                   (ts, mac, dpct, adc, rs, alarm))
    LAST_ALARM[mac] = alarm

    # 把数值一并推送：前端收到就能直接更新卡片 + 追加曲线点，完全不用再发 HTTP。
    # （之前只推 mac，前端只能"每来一帧就 GET 一次 /api/status"，走 Cloudflare 时
    #   一次请求要 1.5 秒以上，请求会堆积成肉眼可见的卡顿。）
    sse_broadcast({"type": "tele", "mac": mac, "ts": ts, "dpct": dpct, "adc": adc,
                   "ao": p.get("ao"), "rs": rs, "rssi": p.get("rssi"),
                   "alarm": alarm, "warmup": 1 if p.get("warmup") else 0,
                   "muted": 1 if p.get("muted") else 0,
                   "label": (dev.get("label") or "")})


def handle_alarm_event(mac, raw):
    p = json.loads(raw)
    ts = int(p.get("ts") or time.time())
    kind = str(p.get("kind", "alarm"))
    dpct = p.get("dpct")
    dpct = float(dpct) if dpct is not None else None

    ensure_device(mac)
    add_event(ts, mac, kind, text=kind, dpct=dpct, rssi=p.get("rssi"))
    if kind == "alarm":
        db_execute("""UPDATE devices SET alarm_count = alarm_count + 1,
                      last_alarm_ts = ? WHERE mac = ?""", (ts, mac))
    log("[%s] %s dpct=%s" % (kind, mac, dpct))
    sse_broadcast({"type": "alarm", "mac": mac, "kind": kind})


def handle_node_state(mac, raw):
    p = json.loads(raw)
    online = bool(p.get("online", True))
    dev = ensure_device(mac)
    sets = ["last_seen = ?", "online = ?", "rssi = ?"]
    args = [int(time.time()), 1 if online else 0, p.get("rssi")]
    for key, col in (("tag", "tag"), ("model", "model"), ("fw", "fw")):
        val = p.get(key)
        if val:
            sets.append("%s = ?" % col)
            args.append(str(val))
    args.append(mac)
    db_execute("UPDATE devices SET %s WHERE mac = ?" % ", ".join(sets), tuple(args))

    if bool(dev.get("online")) != online:
        add_event(int(p.get("ts") or time.time()), mac, "state",
                  text="online" if online else "offline", rssi=p.get("rssi"))
        sse_broadcast({"type": "state", "mac": mac, "online": online})
    else:
        sse_broadcast({"type": "status"})


def on_message(client, userdata, msg):
    global GW_STATE
    try:
        topic = msg.topic
        raw = msg.payload.decode("utf-8", "replace")

        mac = mac_from_topic(topic, "tele")
        if mac:
            handle_telemetry(mac, raw)
            return

        mac = mac_from_topic(topic, "event")
        if mac:
            handle_alarm_event(mac, raw)
            return

        mac = mac_from_topic(topic, "state")
        if mac == "GATEWAY":
            p = json.loads(raw)
            GW_STATE = {"online": bool(p.get("online", True)), "ip": p.get("ip", ""),
                        "wifi_rssi": p.get("wifi_rssi"), "nodes": p.get("nodes", 0),
                        "nodes_max": p.get("nodes_max", 0), "fw": p.get("fw", ""),
                        "ts": int(p.get("ts") or time.time())}
            sse_broadcast({"type": "status"})
            return
        if mac:
            handle_node_state(mac, raw)
            return

        mac = mac_from_topic(topic, "ack")
        if mac:
            p = json.loads(raw)
            add_event(int(p.get("ts") or time.time()), mac, "ack",
                      text="%s -> %s" % (p.get("text", ""), "OK" if p.get("ok") else "FAIL"))
            sse_broadcast({"type": "event"})
    except Exception as exc:
        log("[mqtt] handle error", msg.topic, exc)


def mqtt_start():
    global MQTT_CLIENT
    m = CFG["mqtt"]
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                         client_id="iot-backend", protocol=mqtt.MQTTv311)
    client.username_pw_set(m["backend_user"], m["backend_password"])
    client.on_connect = on_connect
    client.on_message = on_message
    client.reconnect_delay_set(min_delay=1, max_delay=10)
    client.connect_async(m["host"], int(m["port"]), keepalive=30)
    client.loop_start()
    MQTT_CLIENT = client


def mqtt_publish_cmd(mac, text):
    if MQTT_CLIENT is None:
        raise RuntimeError("MQTT 未就绪")
    info = MQTT_CLIENT.publish(TOPIC_BASE + "/cmd/" + mac, text, qos=1, retain=False)
    if info.rc != mqtt.MQTT_ERR_SUCCESS:
        raise RuntimeError("MQTT 发布失败 rc=%s" % info.rc)
    add_event(int(time.time()), mac, "cmd", text=text)
    sse_broadcast({"type": "event"})


def watchdog():
    """网关/节点超时未上报 -> 标记离线"""
    purge_acc = 0
    while True:
        time.sleep(10)
        try:
            # 每 10 分钟清理一次过期数据（曲线按 tele_keep_days，事件按 keep_days）
            purge_acc += 10
            if purge_acc >= 600:
                purge_acc = 0
                db_purge()

            now = int(time.time())
            if GW_STATE["online"] and now - GW_STATE["ts"] > OFFLINE_TIMEOUT:
                GW_STATE["online"] = False
                sse_broadcast({"type": "status"})
            cutoff = now - OFFLINE_TIMEOUT
            for r in db_query("SELECT mac FROM devices WHERE online = 1 AND last_seen < ?",
                              (cutoff,)):
                db_execute("UPDATE devices SET online = 0 WHERE mac = ?", (r["mac"],))
                add_event(now, r["mac"], "state", text="offline")
                sse_broadcast({"type": "status"})
        except Exception as exc:
            log("[watchdog]", exc)


# ---------------------------------------------------------------------------
# HTTP 服务
# ---------------------------------------------------------------------------
class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "SmokeServer/1.0"

    # ---- 工具 ----
    def _json(self, obj, code=200):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        extra = {}
        if len(body) >= GZIP_MIN_BYTES and "gzip" in (self.headers.get("Accept-Encoding") or "").lower():
            body = gz(body)
            extra["Content-Encoding"] = "gzip"
            extra["Vary"] = "Accept-Encoding"
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for k, v in extra.items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def _err(self, code, msg):
        self._json({"detail": msg}, code)

    def _auth(self, query):
        token = self.headers.get("X-Auth") or (query.get("token", [""])[0])
        if token != WEB_PASSWORD:
            self._err(401, "访问口令错误")
            return False
        return True

    def log_message(self, fmt, *args):
        pass          # 静音访问日志, 避免刷屏

    # ---- GET ----
    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path
        query = parse_qs(parsed.query)

        if path.startswith("/api/"):
            if not self._auth(query):
                return
            try:
                if path == "/api/status":
                    return self.api_status()
                if path == "/api/series":
                    return self.api_series(query)
                if path == "/api/events":
                    return self.api_events(query)
                if path == "/api/stats":
                    return self.api_stats(query)
                if path == "/api/stream":
                    return self.api_stream()
                return self._err(404, "未知接口")
            except Exception as exc:
                log("[http]", exc)
                return self._err(500, str(exc))

        return self.serve_static(path)

    def serve_static(self, path):
        if path in ("/", ""):
            path = "/index.html"
        rel = unquote(path).lstrip("/")
        full = os.path.normpath(os.path.join(WEB_DIR, rel))
        if not full.startswith(WEB_DIR) or not os.path.isfile(full):
            self.send_error(404, "Not Found")
            return

        # 静态资源缓存策略（很关键）：
        #   - vendor/ 里的大文件（echarts.min.js，1MB+）允许长缓存，加速手机加载；
        #   - 其它（html/css/js）一律 no-store。
        # 为什么必须 no-store：网页对外走的是 Cloudflare 隧道，而 Cloudflare 默认会按
        # 扩展名缓存 css/js（实测 cf-cache-status: HIT、max-age=14400）。一旦样式更新，
        # 边缘节点可能还在发旧文件，于是出现"域名访问=旧界面、IP 直连=新界面"，
        # 而且不同边缘节点表现还不一样。加上 no-store 后边缘不再缓存，配合
        # index.html 里的 ?v= 版本号，更新样式后刷新即可生效。
        is_vendor = rel.replace("\\", "/").startswith("vendor/")
        cache_control = "public, max-age=604800, immutable" if is_vendor else "no-store, must-revalidate"

        ctype = mimetypes.guess_type(full)[0] or "application/octet-stream"
        if ctype.startswith("text/") or ctype in ("application/javascript", "application/json"):
            ctype += "; charset=utf-8"
        size = os.path.getsize(full)

        # ---- gzip：省流量（这是免费额度下最有用的一招）----
        # 实测：网页未压缩合计 1136 KB，压缩后约 330 KB；
        # 而且走 Cloudflare 隧道时，"服务器 -> 边缘"这一段原本是明文传输的，
        # 在源站压缩后这一段也跟着变小，真正省下的是云服务器的出网流量。
        body = None
        extra = {}
        accepts_gzip = "gzip" in (self.headers.get("Accept-Encoding") or "").lower()
        compressible = ("text/" in ctype) or ("javascript" in ctype) or ("json" in ctype) or ("svg" in ctype)
        if accepts_gzip and compressible and size >= GZIP_MIN_BYTES:
            stamp = (os.path.getmtime(full), size)
            with GZIP_LOCK:
                hit = GZIP_CACHE.get(full)
            if hit and hit[0] == stamp:
                body = hit[1]
            else:
                with open(full, "rb") as fp:
                    body = gz(fp.read())
                with GZIP_LOCK:
                    GZIP_CACHE[full] = (stamp, body)
            extra["Content-Encoding"] = "gzip"
            extra["Vary"] = "Accept-Encoding"

        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body) if body is not None else size))
        self.send_header("Cache-Control", cache_control)
        for k, v in extra.items():
            self.send_header(k, v)
        self.end_headers()
        if body is not None:
            self.wfile.write(body)
            return
        with open(full, "rb") as fp:
            while True:
                chunk = fp.read(64 * 1024)
                if not chunk:
                    break
                self.wfile.write(chunk)

    # ---- API ----
    def api_status(self):
        devices = [{
            "mac": r["mac"], "label": r["label"], "tag": r["tag"], "model": r["model"],
            "fw": r["fw"], "online": bool(r["online"]), "rssi": r["rssi"],
            "last_seen": r["last_seen"], "dpct": r["dpct"], "adc": r["adc"],
            "ao_mv": r["ao_mv"], "rs_ohm": r["rs_ohm"],
            "alarm": bool(r["alarm"]), "warmup": bool(r["warmup"]),
            "muted": bool(r["muted"]), "tele_count": r["tele_count"],
            "alarm_count": r["alarm_count"], "last_alarm_ts": r["last_alarm_ts"],
        } for r in db_query("SELECT * FROM devices ORDER BY label")]
        self._json({"gateway": GW_STATE, "devices": devices,
                    "topic_base": TOPIC_BASE, "max_cmd_len": MAX_CMD_LEN,
                    "tele_store_sec": TELE_STORE_SEC,
                    "cmd_presets": CMD_PRESETS,
                    "now": int(time.time())})

    def api_series(self, query):
        """某个节点(或全部节点)最近 N 小时的 Δ% 曲线

        支持三个参数（前端"曲线每 2 秒刷新"就靠它们）：
          hours      : 时间窗口，可以是小数（0.5 = 近 30 分钟）
          since      : 只取 ts >= since 的增量点（前端缓存 lastTs 后用它增量拉取）
          max_points : 一次最多返回多少个点，超了就等间隔抽稀（报警点始终保留）
        """
        try:
            hours = float(query.get("hours", ["3"])[0])
        except (TypeError, ValueError):
            hours = 3.0
        hours = min(max(hours, 0.1), 168.0)
        window = int(hours * 3600)

        mac = query.get("mac", [""])[0].upper()
        try:
            since = int(query.get("since", ["0"])[0] or 0)
        except (TypeError, ValueError):
            since = 0
        try:
            max_points = int(query.get("max_points", [str(SERIES_MAX_POINTS)])[0])
        except (TypeError, ValueError):
            max_points = SERIES_MAX_POINTS
        max_points = min(max(max_points, 50), 20000)

        # fmt=2 时点用 [ts, dpct, alarm] 数组返回。对象形式一行约 55 字节，
        # 数组只要 ~22 字节，走隧道时更省。（前端已改用 fmt=2；不传仍是老格式。）
        compact = str(query.get("fmt", ["1"])[0]) == "2" and bool(mac)

        now = int(time.time())
        start = max(now - window, since)          # 增量请求时从 since 开始
        incremental = since > 0

        if mac:
            rows = db_query("""SELECT t.ts, t.dpct, t.alarm, d.label FROM telemetry t
                               LEFT JOIN devices d ON d.mac = t.mac
                               WHERE t.mac = ? AND t.ts >= ? ORDER BY t.ts""", (mac, start))
        else:
            rows = db_query("""SELECT t.ts, t.mac, t.dpct, t.alarm, d.label FROM telemetry t
                               LEFT JOIN devices d ON d.mac = t.mac
                               WHERE t.ts >= ? ORDER BY t.ts""", (start,))

        # 抽稀：只在"整段加载"时做；增量请求本身就是新点，不需要处理
        downsampled = False
        if (not incremental) and len(rows) > max_points:
            stride = int(math.ceil(len(rows) / float(max_points)))
            rows = [r for i, r in enumerate(rows) if i % stride == 0 or r.get("alarm")]
            downsampled = True

        if compact:
            out_rows = [[r["ts"], r["dpct"], r["alarm"]] for r in rows]
        else:
            out_rows = rows

        self._json({"hours": hours, "mac": mac, "rows": out_rows, "server_ts": now,
                    "incremental": incremental, "downsampled": downsampled,
                    "compact": compact, "store_sec": TELE_STORE_SEC})

    def api_events(self, query):
        limit = min(max(int(query.get("limit", ["60"])[0]), 1), 500)
        mac = query.get("mac", [""])[0]
        sql = ("SELECT e.*, d.label FROM events e LEFT JOIN devices d ON d.mac = e.mac WHERE 1=1")
        args = []
        if mac:
            sql += " AND e.mac = ?"
            args.append(mac.upper())
        sql += " ORDER BY e.id DESC LIMIT ?"
        args.append(limit)
        self._json({"items": db_query(sql, tuple(args))})

    def api_stats(self, query):
        """近 N 小时每个节点每小时的报警次数（柱状图）"""
        hours = min(max(int(query.get("hours", ["24"])[0]), 1), 168)
        since = int(time.time()) - hours * 3600
        rows = db_query("""SELECT d.label AS label, (e.ts/3600)*3600 AS bucket, COUNT(*) AS n
                           FROM events e LEFT JOIN devices d ON d.mac = e.mac
                           WHERE e.kind='alarm' AND e.ts >= ?
                           GROUP BY d.label, bucket ORDER BY bucket""", (since,))
        labels = [r["label"] or "?" for r in db_query("SELECT label FROM devices ORDER BY label")]
        self._json({"hours": hours, "labels": labels, "rows": rows})

    def api_stream(self):
        q = queue.Queue(maxsize=512)
        with SSE_LOCK:
            SSE_CLIENTS.add(q)
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "close")
        self.end_headers()
        try:
            self.wfile.write(b"retry: 3000\n\n")
            self.wfile.flush()
            while True:
                try:
                    data = q.get(timeout=20)
                    self.wfile.write(("data: %s\n\n" % data).encode("utf-8"))
                except queue.Empty:
                    self.wfile.write(b": ping\n\n")     # 心跳, 防止被隧道/浏览器断开
                self.wfile.flush()
        except Exception:
            pass
        finally:
            with SSE_LOCK:
                SSE_CLIENTS.discard(q)
            self.close_connection = True

    # ---- POST ----
    def do_POST(self):
        parsed = urlparse(self.path)
        query = parse_qs(parsed.query)
        if parsed.path != "/api/cmd":
            return self._err(404, "未知接口")
        if not self._auth(query):
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            body = json.loads(self.rfile.read(length).decode("utf-8") or "{}")
            mac = str(body.get("mac", "")).upper()
            text = str(body.get("text", ""))
            if not mac:
                return self._err(400, "未选择节点")
            if not text:
                return self._err(400, "内容不能为空")
            if len(text) > MAX_CMD_LEN:
                return self._err(400, "最多 %d 个字符" % MAX_CMD_LEN)
            if any(ord(c) < 0x20 or ord(c) > 0x7E for c in text):
                return self._err(400, "只支持数字/字母/常见符号(ASCII)")
            mqtt_publish_cmd(mac, text)
            log("[cmd] -> %s : %s" % (mac, text))
            return self._json({"ok": True, "mac": mac, "text": text})
        except Exception as exc:
            log("[cmd] error", exc)
            return self._err(503, str(exc))


def local_ip_hint():
    """猜一个本机在热点/局域网里的 IP, 方便填到 ESP32 的 app_config.h。"""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "127.0.0.1"


def main():
    db_init()
    mqtt_start()
    threading.Thread(target=watchdog, daemon=True).start()
    srv = ThreadingHTTPServer((HTTP_HOST, HTTP_PORT), Handler)
    srv.daemon_threads = True
    log("网页:  http://127.0.0.1:%d   访问口令: %s" % (HTTP_PORT, WEB_PASSWORD))
    log("本机 IP(填入 ESP32 的 MQTT 地址): %s" % local_ip_hint())
    log("MQTT:  %s:%s  主题前缀: %s" % (CFG["mqtt"]["host"], CFG["mqtt"]["port"], TOPIC_BASE))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        srv.server_close()
        if DB:
            DB.close()


if __name__ == "__main__":
    main()
