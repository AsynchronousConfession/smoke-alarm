#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
用 Cloudflare API 一次性把隧道建好（完全绕开 cloudflared tunnel login 的授权页面）

用法:
    ..\\.venv\\Scripts\\python.exe cf_setup.py <API_TOKEN> [子域名]

它会依次完成:
    1) 校验 API Token
    2) 找到域名所在账号与 Zone ID
    3) 创建（或复用）隧道 iot-home
    4) 把隧道配置设为: iot.<域名> -> http://localhost:3000
    5) 在 DNS 里创建 CNAME: iot.<域名> -> <隧道ID>.cfargotunnel.com（已代理）
    6) 把隧道 Token 写进 ..\\config.json 的 cloudflare.tunnel_token

Token 需要这三条权限:
    Account | Cloudflare Tunnel | Edit
    Zone    | DNS               | Edit
    Zone    | Zone              | Read
"""

import json
import os
import sys
import urllib.error
import urllib.request

API = "https://api.cloudflare.com/client/v4"
DOMAIN = "smoke.example.com"
TUNNEL_NAME = "iot-home"

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CONFIG_PATH = os.path.join(ROOT, "config.json")


def api(method, path, token, body=None):
    req = urllib.request.Request(API + path, method=method)
    req.add_header("Authorization", "Bearer " + token)
    req.add_header("Content-Type", "application/json")
    data = json.dumps(body).encode("utf-8") if body is not None else None
    try:
        with urllib.request.urlopen(req, data=data, timeout=30) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        raw = exc.read().decode("utf-8", "replace")
        try:
            return json.loads(raw)
        except Exception:
            return {"success": False, "errors": [{"message": raw[:300]}]}
    except Exception as exc:
        return {"success": False, "errors": [{"message": str(exc)}]}


def fail(msg, res=None):
    print("✗ " + msg)
    if res:
        print("  API 返回: " + json.dumps(res, ensure_ascii=False)[:600])
    sys.exit(1)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    token = sys.argv[1].strip()
    sub = (sys.argv[2] if len(sys.argv) > 2 else "iot").strip()
    host = "%s.%s" % (sub, DOMAIN)

    print("[1/6] 校验 API Token ...")
    res = api("GET", "/user/tokens/verify", token)
    if not res.get("success"):
        fail("Token 无效或权限不足（如果提示需要验证邮箱，请先在 Cloudflare 里验证邮箱）", res)
    print("      Token 有效 ✓")

    print("[2/6] 查找域名 %s ..." % DOMAIN)
    res = api("GET", "/zones?name=" + DOMAIN, token)
    if not res.get("success") or not res.get("result"):
        fail("找不到域名（确认 Token 的 Zone 权限包含这个域名）", res)
    zone = res["result"][0]
    zone_id = zone["id"]
    account_id = zone["account"]["id"]
    print("      Zone ID    = %s" % zone_id)
    print("      Account ID = %s" % account_id)

    print("[3/6] 创建/复用隧道 %s ..." % TUNNEL_NAME)
    res = api("GET", "/accounts/%s/cfd_tunnel?name=%s&is_deleted=false" % (account_id, TUNNEL_NAME), token)
    tunnel = None
    if res.get("success") and res.get("result"):
        tunnel = res["result"][0]
        print("      已存在，复用: %s" % tunnel["id"])

    if tunnel is None:
        res = api("POST", "/accounts/%s/cfd_tunnel" % account_id, token,
                  {"name": TUNNEL_NAME, "config_src": "cloudflare"})
        if not res.get("success"):
            fail("创建隧道失败", res)
        tunnel = res["result"]
        print("      创建成功: %s" % tunnel["id"])

    tunnel_id = tunnel["id"]
    tunnel_token = tunnel.get("token")
    if not tunnel_token:
        res = api("GET", "/accounts/%s/cfd_tunnel/%s/token" % (account_id, tunnel_id), token)
        if res.get("success"):
            tunnel_token = res.get("result")
    if not tunnel_token:
        fail("没拿到隧道 Token（请到 Zero Trust 控制台里查看）")

    print("[4/6] 配置隧道路由 %s -> http://localhost:3000 ..." % host)
    cfg = {"config": {"ingress": [
        {"hostname": host, "service": "http://localhost:3000"},
        {"service": "http_status:404"},
    ]}}
    res = api("PUT", "/accounts/%s/cfd_tunnel/%s/configurations" % (account_id, tunnel_id), token, cfg)
    if not res.get("success"):
        fail("设置隧道配置失败", res)
    print("      路由配置完成 ✓")

    print("[5/6] 创建 DNS 记录 %s ..." % host)
    target = "%s.cfargotunnel.com" % tunnel_id
    body = {"type": "CNAME", "name": host, "content": target, "proxied": True, "ttl": 1}
    res = api("POST", "/zones/%s/dns_records" % zone_id, token, body)
    if not res.get("success"):
        msg = json.dumps(res, ensure_ascii=False)
        if "already exists" in msg:
            print("      记录已存在，改为更新 ...")
            recs = api("GET", "/zones/%s/dns_records?name=%s" % (zone_id, host), token)
            if recs.get("success") and recs.get("result"):
                rid = recs["result"][0]["id"]
                res = api("PUT", "/zones/%s/dns_records/%s" % (zone_id, rid), token, body)
        if not res.get("success"):
            fail("创建 DNS 记录失败", res)
    print("      CNAME %s -> %s (已代理) ✓" % (host, target))

    print("[6/6] 写入 config.json ...")
    with open(CONFIG_PATH, "r", encoding="utf-8") as fp:
        cfg_json = json.load(fp)
    cfg_json.setdefault("cloudflare", {})["tunnel_token"] = tunnel_token
    with open(CONFIG_PATH, "w", encoding="utf-8") as fp:
        json.dump(cfg_json, fp, ensure_ascii=False, indent=2)
    print("      已写入 %s" % CONFIG_PATH)

    print("\n全部完成 ✓  接下来:")
    print("  1) 重新运行 .\\start.ps1   （会自动把隧道一起拉起来）")
    print("  2) 手机用流量打开 https://%s" % host)
    print("  3) 输入网页口令 %s" % cfg_json["web"]["access_password"])


if __name__ == "__main__":
    main()
