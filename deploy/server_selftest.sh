#!/usr/bin/env bash
# 云服务器端到端自检
#
# 在服务器上直接跑：
#     sudo bash /opt/smoke-alarm/server_selftest.sh
# 或从本机通过 ssh 送进去跑（不需要先上传）：
#     Get-Content .\deploy\server_selftest.sh -Raw | ssh -i $key root@192.0.2.10 "tr -d '\r' | bash -s"
#
# 它会：用设备账号发测试数据 → 查后端是否入库 → 验证 ACL 越权被拦 → 清理测试数据
set -uo pipefail
cd /opt/smoke-alarm || exit 1

DP=$(python3 -c "import json;print(json.load(open('config.json'))['mqtt']['device_password'])")
DU=$(python3 -c "import json;print(json.load(open('config.json'))['mqtt']['device_user'])")
WP=$(python3 -c "import json;print(json.load(open('config.json'))['web']['access_password'])")
MAC=84C2E4TEST01

echo "--- 1) 用设备账号发布测试数据 ---"
cat > /tmp/tele.json <<EOF
{"ts":0,"mac":"$MAC","seq":1,"adc":1234,"ao":999,"rs":4004,"dpct":-52.3,"alarm":1,"warmup":0,"do":0,"muted":0,"rssi":-55}
EOF
cat > /tmp/state.json <<EOF
{"ts":0,"mac":"$MAC","online":true,"rssi":-55,"tag":"SELFTEST","model":"MQ-2","fw":"1.0"}
EOF
cat > /tmp/gw.json <<EOF
{"ts":0,"online":true,"ip":"192.0.2.10","wifi_rssi":-50,"nodes":1,"nodes_max":4,"fw":"1.0"}
EOF

mosquitto_pub -h 127.0.0.1 -u "$DU" -P "$DP" -t "home/gw01/tele/$MAC"    -f /tmp/tele.json  && echo "   遥测发布 OK"
mosquitto_pub -h 127.0.0.1 -u "$DU" -P "$DP" -t "home/gw01/state/$MAC"   -f /tmp/state.json && echo "   节点状态发布 OK"
mosquitto_pub -h 127.0.0.1 -u "$DU" -P "$DP" -t "home/gw01/state/gateway" -f /tmp/gw.json  && echo "   网关心跳发布 OK"

sleep 1
echo "--- 2) 后端是否收到（/api/status）---"
curl -s -H "X-Auth: $WP" http://127.0.0.1:3000/api/status | python3 -m json.tool | head -28

echo "--- 3) ACL 越权测试：设备账号尝试【订阅】遥测主题（应当被拒绝）---"
if timeout 3 mosquitto_sub -h 127.0.0.1 -u "$DU" -P "$DP" -t "home/gw01/tele/$MAC" -C 1 >/dev/null 2>&1; then
    echo "   ！警告：设备账号竟然能订阅遥测主题，ACL 可能没生效"
else
    echo "   OK：被 ACL 拦住了（设备账号只能写 tele、只能读 cmd）"
fi

echo "--- 4) 清理测试数据 ---"
python3 - <<'PY'
import sqlite3
db = sqlite3.connect('/opt/smoke-alarm/data/iot.db')
for t in ('telemetry', 'events', 'devices'):
    db.execute("DELETE FROM %s WHERE mac = '84C2E4TEST01'" % t)
db.commit()
print("   已删除测试节点 84C2E4TEST01 的数据")
db.close()
PY

echo "--- 5) 服务与端口汇总 ---"
systemctl is-active smoke-mosquitto smoke-backend | tr '\n' ' '; echo
ss -lntp | grep -E ":1883|:3000" | sed 's/^/   /'
