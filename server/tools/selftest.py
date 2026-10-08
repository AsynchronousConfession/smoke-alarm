#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
链路自检：不接 ESP32 / 不接节点，也能验证 【MQTT -> 数据库 -> 网页】 与 【网页 -> MQTT】 是否通。

用法（先在 server 目录跑 .\\start.ps1，再另开一个窗口）:
    cd <工程>\server\tools
    ..\\.venv\\Scripts\\python.exe selftest.py

它会:
  1) 用设备账号连上 Mosquitto，扮演一个节点发布: 遥测帧 + 报警事件 + 在线状态 + 网关心跳
  2) 问后端接口，确认这些数据已经入库（说明 MQTT、数据库、网页全通）
  3) 订阅 home/<网关ID>/cmd/+，等你 20 秒
     —— 这 20 秒内你去网页点一次“发送”，这里就会打印收到的指令（说明下行也通）
"""

import json
import os
import sys
import time
import urllib.error
import urllib.request

import paho.mqtt.client as mqtt

# Windows 中文控制台是 GBK, 个别符号编码不了会让脚本崩掉, 这里只做容错
try:
    sys.stdout.reconfigure(errors="replace")
except Exception:
    pass

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
with open(os.path.join(ROOT, "config.json"), "r", encoding="utf-8") as fp:
    CFG = json.load(fp)

GW = CFG["gateway_id"]
BASE = "home/%s" % GW
TEST_MAC = "84C2E4FFFF01"                       # 自检用假 MAC
WEB = "http://127.0.0.1:%d" % int(CFG["web"]["port"])
PASS = CFG["web"]["access_password"]


def http_get(path):
    req = urllib.request.Request(WEB + path)
    req.add_header("X-Auth", PASS)
    with urllib.request.urlopen(req, timeout=5) as r:
        return json.loads(r.read().decode("utf-8"))


got_cmd = []


def on_connect(c, u, flags, rc, props=None):
    print("[1] MQTT 连接成功")
    c.subscribe(BASE + "/cmd/+", qos=1)

    now = int(time.time())
    # 节点信息（型号/编号）
    c.publish(BASE + "/state/" + TEST_MAC,
              json.dumps({"ts": now, "mac": TEST_MAC, "online": True, "rssi": -55,
                          "tag": "TEST", "model": "MQ-2", "fw": "1.0"}),
              qos=1, retain=True)
    # 一帧遥测：Δ% = -52.3（明显有烟）
    c.publish(BASE + "/tele/" + TEST_MAC,
              json.dumps({"ts": now, "mac": TEST_MAC, "seq": 1, "adc": 1234, "ao": 999,
                          "rs": 4004, "dpct": -52.3, "alarm": 1, "warmup": 0,
                          "do": 0, "muted": 0, "rssi": -55}), qos=1)
    # 报警事件
    c.publish(BASE + "/event/" + TEST_MAC,
              json.dumps({"ts": now, "mac": TEST_MAC, "kind": "alarm", "dpct": -52.3,
                          "rs": 4004, "adc": 1234, "rssi": -55}), qos=1)
    # 网关心跳
    c.publish(BASE + "/state/gateway",
              json.dumps({"ts": now, "online": True, "ip": "127.0.0.1",
                          "wifi_rssi": -50, "nodes": 1, "nodes_max": 4, "fw": "1.0"}),
              qos=1, retain=True)
    print("[2] 已发布测试用遥测 / 报警事件 / 网关心跳")


def on_message(c, u, msg):
    got_cmd.append((msg.topic, msg.payload.decode("utf-8", "replace")))
    print("    >> 收到下行指令:", msg.topic, "=", msg.payload.decode("utf-8", "replace"))


def main():
    m = CFG["mqtt"]
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="selftest",
                    protocol=mqtt.MQTTv311)
    c.username_pw_set(m["device_user"], m["device_password"])
    c.on_connect = on_connect
    c.on_message = on_message
    try:
        c.connect(m["host"], int(m["port"]), keepalive=15)
    except Exception as exc:
        print("!! 连不上 MQTT，请确认 start.ps1 已经跑起来：", exc)
        return 1
    c.loop_start()
    time.sleep(2)

    ok = True

    print("[3] 检查后端是否收到（网页上应该多出一个 TEST / MQ-2 的节点）")
    try:
        ev = http_get("/api/events?limit=20")
        hit = [i for i in ev["items"] if i["mac"] == TEST_MAC and i["kind"] == "alarm"]
        if hit:
            print("    [OK]   数据库里已找到报警事件: dpct=%s" % hit[0]["dpct"])
        else:
            print("    [FAIL] 数据库没找到，检查后端日志 logs\\backend.err.log")
            ok = False

        st = http_get("/api/status")
        dev = [d for d in st["devices"] if d["mac"] == TEST_MAC]
        if dev and dev[0]["model"] == "MQ-2":
            print("    [OK]   节点已出现在状态接口: %s / %s" % (dev[0]["label"], dev[0]["model"]))
        else:
            print("    [FAIL] 状态接口里没有这个节点")
            ok = False
        if st["gateway"]["online"]:
            print("    [OK]   网关心跳已入库")
        else:
            print("    [FAIL] 网关状态没更新")
            ok = False
    except Exception as exc:
        print("    [FAIL] 打不开后端接口:", exc)
        ok = False

    print("\n[4] 接下来 20 秒内，去网页上点一次“发送”，验证下行通道…")
    time.sleep(20)
    if got_cmd:
        print("[OK] 下行通道正常，网页发的指令已经送到 MQTT")
    else:
        print("[FAIL] 没收到下行指令（后端没在运行，或网页没点发送）")
        ok = False

    c.loop_stop()
    print("\n结果:", "全部通过" if ok else "有失败项")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
