#!/usr/bin/env bash
# =============================================================================
#  云服务器流量日报（阿里云入网免费、出网计费，所以重点看"出网"）
#
#  用法：
#     sudo bash traffic_report.sh          # 记录一条并打印最近 7 天
#     sudo bash traffic_report.sh --show   # 只看历史，不记录
#
#  安装（每天 23:59 自动记录）：
#     sudo cp traffic_report.sh /usr/local/bin/smoke-traffic
#     sudo chmod +x /usr/local/bin/smoke-traffic
#     (crontab -l 2>/dev/null; echo "59 23 * * * /usr/local/bin/smoke-traffic >/dev/null 2>&1") | crontab -
# =============================================================================
set -uo pipefail

STATE=/var/lib/smoke-traffic.state
LOG=/var/log/smoke-traffic.log
IFACE=$(ip route 2>/dev/null | awk '/^default/ {print $5; exit}')
[ -z "${IFACE:-}" ] && IFACE=eth0

if [[ "${1:-}" != "--show" ]]; then
    read -r RX TX < <(awk -v i="$IFACE:" '$1 == i {print $2, $10}' /proc/net/dev)
    if [ -f "$STATE" ]; then
        read -r PRX PTX < "$STATE"
        [ "$RX" -lt "$PRX" ] && PRX=0        # 计数器回绕保护
        [ "$TX" -lt "$PTX" ] && PTX=0
        echo "$(date '+%F %T')  出网 $(( (TX - PTX) / 1048576 )) MB   入网 $(( (RX - PRX) / 1048576 )) MB   （累计出网 $(( TX / 1073741824 )) GB / 入网 $(( RX / 1073741824 )) GB）" >> "$LOG"
    fi
    echo "$RX $TX" > "$STATE"
    echo "已记录：$(tail -1 "$LOG" 2>/dev/null || echo '（第一次运行，只建了基线，下次才有对比）')"
fi

echo
echo "===== 最近 7 条记录 ====="
tail -7 "$LOG" 2>/dev/null || echo "（还没有历史记录）"
echo
echo "===== 本月累计（按记录求和）====="
awk -v m="$(date '+%Y-%m')" '$0 ~ m {
    for (i = 1; i <= NF; i++) {
        if ($i == "出网") out += $(i + 1);
        if ($i == "入网") inn += $(i + 1);
    }
} END { printf "  出网合计 %d MB（%.2f GB）   入网合计 %d MB\n", out, out / 1024, inn }' "$LOG" 2>/dev/null
echo
echo "提示：阿里云「入网流量免费、出网流量计费」；出网的大头通常是网页里的 echarts.min.js（1.1 MB，已开启 gzip → 369 KB）。"
