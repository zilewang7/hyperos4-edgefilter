#!/system/bin/sh
# 由 KernelSU 在 late_start service 阶段以 root 执行
MODDIR=${0%/*}

# 首次运行时铺一份可编辑配置
mkdir -p /data/adb/edgefilter
[ -f /data/adb/edgefilter/config ] || cp "$MODDIR/config" /data/adb/edgefilter/config

# 等系统起完再抓设备，避免抓到一半
i=0
while [ "$(getprop sys.boot_completed)" != "1" ] && [ $i -lt 120 ]; do
    sleep 1
    i=$((i + 1))
done
sleep 2

# 同步 Web 控制台文件
if [ -d "$MODDIR/web" ]; then
    mkdir -p /data/adb/edgefilter/web
    cp -rf "$MODDIR/web/." /data/adb/edgefilter/web/
    chmod 755 /data/adb/edgefilter/web/cgi-bin/api
    chmod 644 /data/adb/edgefilter/web/index.html
fi

"$MODDIR/ctl.sh" start >> /data/adb/edgefilter/edgefilter.log 2>&1

# 亮屏看护（先清掉旧实例，避免重复）
pkill -f "modules/edgefilter/watch.sh" 2>/dev/null
nohup "$MODDIR/watch.sh" >> /data/adb/edgefilter/edgefilter.log 2>&1 &
"$MODDIR/web.sh" start >> /data/adb/edgefilter/edgefilter.log 2>&1
