#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
曲线刷新速率自检：量一量"数据从节点到网页曲线"到底有多快。

用法（先在 server 目录跑 start.ps1）：
    cd <工程>\server\tools
    ..\\.venv\\Scripts\\python.exe check_speed.py

它会：
  1) 等 24 秒，让新频率的数据攒起来；
  2) 拉一次近 30 分钟的曲线，报告点数与相邻点的时间间隔（=实际入库密度）；
  3) 用 since 做一次增量拉取，验证"只取新点"确实生效；
  4) 测一下近 24 小时长时段的抽稀是否正常。
"""

import json
import statistics
import sys
import time
import urllib.request

try:
    sys.stdout.reconfigure(errors="replace")
except Exception:
    pass

import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
with open(os.path.join(ROOT, "config.json"), "r", encoding="utf-8") as fp:
    CFG = json.load(fp)

BASE = "http://127.0.0.1:%d" % int(CFG["web"]["port"])
PASS = CFG["web"]["access_password"]


def get(path):
    req = urllib.request.Request(BASE + path)
    req.add_header("X-Auth", PASS)
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.loads(r.read().decode("utf-8"))


def main():
    st = get("/api/status")
    if not st["devices"]:
        print("还没有节点数据，先让节点上报一会儿再测。")
        return 1
    mac = st["devices"][0]["mac"]
    print("测试节点: %s (%s)" % (mac, st["devices"][0].get("model") or "?"))

    print("\n[1] 等待 24 秒，让新频率的数据攒起来…")
    time.sleep(24)

    d = get("/api/series?mac=%s&hours=0.5&max_points=2000" % mac)
    rows = d["rows"]
    print("[2] 近 30 分钟曲线: %d 个点；后端设定的入库间隔 = %s 秒" % (len(rows), d.get("store_sec")))
    ts = [r["ts"] for r in rows][-12:]
    gaps = [b - a for a, b in zip(ts, ts[1:])]
    if gaps:
        print("    最后 12 个点的时间间隔: %s 秒" % gaps)
        print("    => 实测曲线分辨率 ≈ %.1f 秒/点" % statistics.median(gaps))

    last_ts = rows[-1]["ts"]
    print("\n[3] 增量拉取测试: since=%d，等 8 秒…" % last_ts)
    time.sleep(8)
    inc = get("/api/series?mac=%s&hours=0.5&since=%d" % (mac, last_ts + 1))
    print("    增量返回 %d 个新点: %s" % (
        len(inc["rows"]), [(r["ts"] - last_ts, r["dpct"]) for r in inc["rows"]]))

    d2 = get("/api/series?mac=%s&hours=0.5&max_points=2000" % mac)
    print("    整段点数 %d -> %d（说明曲线在实时增长）" % (len(rows), len(d2["rows"])))

    print("\n[4] 长时段抽稀测试:")
    d3 = get("/api/series?mac=%s&hours=24&max_points=1500" % mac)
    print("    近 24 小时: 返回 %d 点，downsampled=%s" % (len(d3["rows"]), d3["downsampled"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
