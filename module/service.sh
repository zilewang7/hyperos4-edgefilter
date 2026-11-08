#!/system/bin/sh
# KernelSU late_start service；也是模块按钮使用的唯一完整启动入口。
MODDIR=${0%/*}
mkdir -p /data/adb/edgefilter
[ -f /data/adb/edgefilter/config ] || cp "$MODDIR/config" /data/adb/edgefilter/config

i=0
while [ "$(getprop sys.boot_completed)" != "1" ] && [ $i -lt 120 ]; do
    sleep 1
    i=$((i + 1))
done
sleep 2

# HTTP 控制台使用运行目录副本；KSU WebUI 直接读模块 webroot。
if [ -d "$MODDIR/web" ]; then
    mkdir -p /data/adb/edgefilter/web
    cp -rf "$MODDIR/web/." /data/adb/edgefilter/web/
    chmod 755 /data/adb/edgefilter/web/cgi-bin/api
    chmod 644 /data/adb/edgefilter/web/index.html
fi

"$MODDIR/ctl.sh" start >> /data/adb/edgefilter/edgefilter.log 2>&1
"$MODDIR/web.sh" start >> /data/adb/edgefilter/edgefilter.log 2>&1
