#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
端到端冒烟测试（不需要装 Mosquitto，也不需要 ESP32）

自带一个最小 MQTT 3.1.1 broker，然后：
    扮演 ESP32 网关 -> 上报按键事件 -> 检查后端数据库/接口
    扮演网页        -> 下发指令     -> 检查设备侧能不能收到

用法:
    ..\.venv\Scripts\python.exe mini_broker_test.py

跑通说明 PC 端代码与主题约定都没问题；之后再装 Mosquitto 就是换成真 broker 而已。
"""

import json
import os
import socket
import struct
import subprocess
import sys
import threading
import time
import urllib.request

import paho.mqtt.client as mqtt

# Windows 中文控制台是 GBK, 个别符号编码不了会让脚本崩掉, 这里只做容错
try:
    sys.stdout.reconfigure(errors="replace")
except Exception:
    pass

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
VENV_PY = os.path.join(ROOT, ".venv", "Scripts", "python.exe")

with open(os.path.join(ROOT, "config.json"), "r", encoding="utf-8") as fp:
    CFG = json.load(fp)

PORT = int(CFG["mqtt"]["port"])
WEB = "http://127.0.0.1:%d" % int(CFG["web"]["port"])
PASS = CFG["web"]["access_password"]
BASE = "home/%s" % CFG["gateway_id"]
MAC = "84C2E4FFFF02"


# ---------------------------------------------------------------------------
# 最小 MQTT broker
# ---------------------------------------------------------------------------
def read_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("closed")
        buf += chunk
    return buf


def read_remaining_length(sock):
    value = 0
    mult = 1
    while True:
        b = read_exact(sock, 1)[0]
        value += (b & 0x7F) * mult
        if not (b & 0x80):
            return value
        mult *= 128


def encode_remaining_length(n):
    out = b""
    while True:
        b = n % 128
        n //= 128
        if n:
            b |= 0x80
        out += bytes([b])
        if not n:
            return out


def topic_matches(filt, topic):
    f, t = filt.split("/"), topic.split("/")
    for i, part in enumerate(f):
        if part == "#":
            return True
        if i >= len(t):
            return False
        if part != "+" and part != t[i]:
            return False
    return len(f) == len(t)


class MiniBroker:
    def __init__(self, port):
        self.port = port
        self.lock = threading.Lock()
        self.clients = []          # [(sock, subs{filter: qos})]
        self.running = True
        self.srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", port))
        self.srv.listen(8)

    def serve(self):
        while self.running:
            try:
                conn, _ = self.srv.accept()
            except OSError:
                return
            threading.Thread(target=self.handle, args=(conn,), daemon=True).start()

    def handle(self, conn):
        subs = {}
        with self.lock:
            self.clients.append((conn, subs))
        try:
            while self.running:
                hdr = read_exact(conn, 1)[0]
                ptype, flags = hdr >> 4, hdr & 0x0F
                length = read_remaining_length(conn)
                body = read_exact(conn, length) if length else b""

                if ptype == 1:                       # CONNECT
                    conn.sendall(b"\x20\x02\x00\x00")
                elif ptype == 3:                     # PUBLISH
                    qos = (flags >> 1) & 0x03
                    tlen = struct.unpack("!H", body[:2])[0]
                    topic = body[2:2 + tlen].decode()
                    idx = 2 + tlen
                    pid = None
                    if qos > 0:
                        pid = struct.unpack("!H", body[idx:idx + 2])[0]
                        idx += 2
                    payload = body[idx:]
                    if qos == 1 and pid is not None:
                        conn.sendall(b"\x40\x02" + struct.pack("!H", pid))
                    self.deliver(topic, payload)
                elif ptype == 8:                     # SUBSCRIBE
                    pid = struct.unpack("!H", body[:2])[0]
                    i, granted = 2, []
                    while i < len(body):
                        tlen = struct.unpack("!H", body[i:i + 2])[0]
                        flt = body[i + 2:i + 2 + tlen].decode()
                        qos = body[i + 2 + tlen]
                        subs[flt] = qos
                        granted.append(qos)
                        i += 3 + tlen
                    conn.sendall(b"\x90" + encode_remaining_length(2 + len(granted)) +
                                 struct.pack("!H", pid) + bytes(granted))
                elif ptype == 12:                    # PINGREQ
                    conn.sendall(b"\xd0\x00")
                elif ptype == 14:                    # DISCONNECT
                    break
        except Exception:
            pass
        finally:
            with self.lock:
                self.clients = [c for c in self.clients if c[0] is not conn]
            try:
                conn.close()
            except Exception:
                pass

    def deliver(self, topic, payload):
        data = topic.encode()
        pkt = b"\x30" + encode_remaining_length(2 + len(data) + len(payload)) + \
              struct.pack("!H", len(data)) + data + payload
        with self.lock:
            targets = [(c, s) for c, s in self.clients
                       if any(topic_matches(f, topic) for f in s)]
        for c, _ in targets:
            try:
                c.sendall(pkt)
            except Exception:
                pass

    def stop(self):
        self.running = False
        try:
            self.srv.close()
        except Exception:
            pass


# ---------------------------------------------------------------------------
# 测试
# ---------------------------------------------------------------------------
def http(path, method="GET", body=None):
    req = urllib.request.Request(WEB + path, method=method)
    req.add_header("X-Auth", PASS)
    data = None
    if body is not None:
        data = json.dumps(body).encode()
        req.add_header("Content-Type", "application/json")
    with urllib.request.urlopen(req, data=data, timeout=6) as r:
        return json.loads(r.read().decode("utf-8"))


def wait_port(port, timeout=15):
    end = time.time() + timeout
    while time.time() < end:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1):
                return True
        except OSError:
            time.sleep(0.3)
    return False


def main():
    ok = True
    broker = MiniBroker(PORT)
    threading.Thread(target=broker.serve, daemon=True).start()
    print("[1] 迷你 MQTT broker 已监听 127.0.0.1:%d" % PORT)

    backend = subprocess.Popen([VENV_PY, "-u", os.path.join(ROOT, "backend", "app.py")],
                               cwd=os.path.join(ROOT, "backend"),
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if not wait_port(int(CFG["web"]["port"])):
        print("!! 后端启动失败")
        broker.stop(); backend.terminate()
        return 1
    print("[2] 后端已启动，端口", CFG["web"]["port"])
    time.sleep(1.5)

    got = []
    dev = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="esp32gw-test",
                      protocol=mqtt.MQTTv311)
    dev.on_message = lambda c, u, m: got.append((m.topic, m.payload.decode()))
    dev.connect("127.0.0.1", PORT, 15)
    dev.loop_start()
    dev.subscribe(BASE + "/cmd/+", 1)
    time.sleep(0.8)

    print("[3] 模拟 ESP32 上报节点遥测与报警事件")
    dev.publish(BASE + "/tele/" + MAC,
                json.dumps({"ts": int(time.time()), "mac": MAC, "seq": 1, "adc": 1234,
                            "ao": 999, "rs": 4004, "dpct": -52.3, "alarm": 1,
                            "warmup": 0, "do": 0, "muted": 0, "rssi": -48}), qos=1)
    dev.publish(BASE + "/event/" + MAC,
                json.dumps({"ts": int(time.time()), "mac": MAC, "kind": "alarm",
                            "dpct": -52.3, "rs": 4004, "adc": 1234, "rssi": -48}), qos=1)
    dev.publish(BASE + "/state/gateway",
                json.dumps({"ts": int(time.time()), "online": True,
                            "ip": "192.168.137.1", "wifi_rssi": -50,
                            "nodes": 1, "nodes_max": 4}), qos=1)
    time.sleep(1.5)

    ev = http("/api/events?limit=20")
    hit = [i for i in ev["items"] if i["mac"] == MAC and i["kind"] == "alarm"]
    if hit:
        print("    [OK]   上行通过：数据库已写入报警事件 dpct=%s" % hit[0]["dpct"])
    else:
        print("    [FAIL] 上行失败：数据库里没有这条事件")
        ok = False

    st = http("/api/status")
    if st["gateway"]["online"] and any(d["mac"] == MAC for d in st["devices"]):
        print("    [OK]   设备与网关心跳都已在网页接口里可见")
    else:
        print("    [FAIL] 状态接口数据不对")
        ok = False

    print("[4] 模拟网页下发指令 Hello1")
    http("/api/cmd", "POST", {"mac": MAC, "text": "Hello1"})
    time.sleep(1.5)
    if any(m[1] == "Hello1" for m in got):
        print("    [OK]   下行通过：设备侧收到 \"Hello1\"")
    else:
        print("    [FAIL] 下行失败：设备侧没收到指令", got)
        ok = False

    dev.loop_stop()
    dev.disconnect()
    backend.terminate()
    broker.stop()
    print("\n结果:", "全部通过" if ok else "有失败项")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
