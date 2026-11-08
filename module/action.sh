#!/system/bin/sh
# KernelSU 模块卡片「操作」：走与开机相同的唯一启动入口。
MODDIR=${0%/*}
CFG=/data/adb/edgefilter/config
[ -f "$CFG" ] && . "$CFG"
PORT=${WEB_PORT:-8089}

sh "$MODDIR/service.sh"
IP=$(ip route get 1.1.1.1 2>/dev/null | sed -n 's/.* src \([0-9.]*\).*/\1/p' | head -n1)

echo "===== EdgeFilter 边缘防误触 ====="
"$MODDIR/ctl.sh" status
"$MODDIR/web.sh" status
echo "Web 控制台: http://${IP:-<设备IP>}:${PORT}/"
echo "模块卡片点「打开」可直接进 WebUI（走 ksu.exec，无需端口）"
