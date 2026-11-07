#!/system/bin/sh
# KernelSU 模块卡片「操作」按钮：打印状态并确保服务在跑
MODDIR=${0%/*}
CFG=/data/adb/edgefilter/config
[ -f "$CFG" ] && . "$CFG"
PORT=${WEB_PORT:-8089}

IP=$(ip route get 1.1.1.1 2>/dev/null | sed -n 's/.* src \([0-9.]*\).*/\1/p' | head -n1)
[ -z "$IP" ] && IP=$(ip -4 addr show wlan0 2>/dev/null | sed -n 's/.*inet \([0-9.]*\).*/\1/p' | head -n1)

echo "===== EdgeFilter 边缘防误触 ====="
"$MODDIR/ctl.sh" status
"$MODDIR/web.sh" status

if ! "$MODDIR/ctl.sh" status | grep -q running; then
    echo "-> 启动过滤服务"
    "$MODDIR/ctl.sh" start
fi
if ! "$MODDIR/web.sh" status | grep -q running; then
    echo "-> 启动 Web 控制台"
    "$MODDIR/web.sh" start
fi

echo ""
echo "Web 控制台: http://${IP:-<设备IP>}:${PORT}/"
echo "模块卡片点「打开」可直接进 WebUI（走 ksu.exec，无需端口）"
