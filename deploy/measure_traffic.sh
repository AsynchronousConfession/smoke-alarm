#!/usr/bin/env bash
# 测 180 秒的空闲流量，并按进程归因（需要 nethogs）
set -uo pipefail
iface=eth0

read -r rx1 tx1 <<< "$(awk -v i="$iface:" '$1 == i {print $2, $10}' /proc/net/dev)"

nethogs -t -d 10 "$iface" > /tmp/nethogs.txt 2>&1 &
nhpid=$!
sleep 180
kill "$nhpid" 2>/dev/null || true
wait "$nhpid" 2>/dev/null || true

read -r rx2 tx2 <<< "$(awk -v i="$iface:" '$1 == i {print $2, $10}' /proc/net/dev)"
echo "=== 180 秒空闲流量 ==="
echo "  出网: $(( (tx2-tx1)/1024 )) KB  =>  约 $(( (tx2-tx1)*480/1048576 )) MB/天"
echo "  入网: $(( (rx2-rx1)/1024 )) KB  =>  约 $(( (rx2-rx1)*480/1048576 )) MB/天"

echo "=== 按进程归因（单位 KB/s，取各次采样的最大值）==="
awk -F'\t' 'NF == 3 && $2+0 > 0 {
    split($1, p, "/");
    name = p[1];
    if ($2 + 0 > max[name]) max[name] = $2 + 0;
} END {
    for (n in max) printf "  %-52s 峰值 %8.2f KB/s\n", n, max[n];
}' /tmp/nethogs.txt | sort -k3 -nr

echo "=== 当前隧道连接（应为 1 条 http2）==="
ss -uanp 2>/dev/null | grep -c cloudflared | sed 's/^/   cloudflared 的 UDP 套接字数: /'
journalctl -u cloudflared -n 30 --no-pager | grep -c "protocol=http2" | sed 's/^/   最近日志里 http2 连接记录: /'
