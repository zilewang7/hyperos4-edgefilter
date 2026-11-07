#!/system/bin/sh
[ -f /data/adb/edgefilter/edgefilter.pid ] && kill "$(cat /data/adb/edgefilter/edgefilter.pid)" 2>/dev/null
exit 0
