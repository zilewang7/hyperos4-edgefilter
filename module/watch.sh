#!/system/bin/sh
# 亮灭屏 + 旋转看护
#   亮屏   -> SIGHUP : 重新 grab 面板 + 新建虚拟设备 + 清空触点状态 + 重读旋转
#   灭屏   -> SIGUSR1: 释放 grab + 删除虚拟设备，把手势唤醒完全交还原生路径
#   旋转变 -> SIGUSR2: 重读旋转
#
# 旋转检测放在这个脚本里做（dumpsys input 一次约 45ms），只有值真的变了才通知
# daemon，避免把开销和阻塞带进 daemon 的触摸事件循环。
# 两路来源：configuration_changed 事件（转屏时系统会打，响应快）+ 4 秒轮询兜底。
MODDIR=${0%/*}
PIDF=/data/adb/edgefilter/edgefilter.pid
LOG=/data/adb/edgefilter/edgefilter.log

signal() {          # $1 = signal name
    pid=$(cat "$PIDF" 2>/dev/null)
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        kill -"$1" "$pid" 2>/dev/null
    else
        "$MODDIR/ctl.sh" start >> "$LOG" 2>&1
    fi
}

cur_rot() {
    dumpsys input 2>/dev/null | grep -m1 'Viewport INTERNAL' \
        | sed -n 's/.*orientation=\([0-9]\).*/\1/p'
}

# 启动时先对齐一次当前状态（避免开机时屏幕是灭的却还抓着面板）
case "$(dumpsys power 2>/dev/null | grep -m1 'mWakefulness=')" in
    *Awake*) signal HUP ;;
    *)       signal USR1 ;;
esac

# 兜底轮询：每 2 秒对一次
(
    last=$(cur_rot)
    while :; do
        sleep 2
        r=$(cur_rot)
        [ -z "$r" ] && continue
        if [ "$r" != "$last" ]; then
            last=$r
            signal USR2
        fi
    done
) &

# 事件快路径。logcat 会先把整个 events buffer 倒出来再跟随，
# 所以先数出历史行数，用 tail -n +N 跳过，否则会瞬间触发几百次。
N=$(logcat -b events -d -s screen_toggled configuration_changed 2>/dev/null | wc -l)
logcat -b events -s screen_toggled configuration_changed 2>/dev/null \
| tail -n +$((N + 1)) \
| while IFS= read -r line; do
    case "$line" in
        *"screen_toggled: 1"*)  signal HUP ;;
        *"screen_toggled: 0"*)  signal USR1 ;;
        *"configuration_changed"*)
            # 这里自己比对一次，只有真的变了才叫 daemon
            r=$(cur_rot)
            [ -z "$r" ] && continue
            if [ "$r" != "$last_ev" ]; then
                last_ev=$r
                signal USR2
            fi
            ;;
    esac
done
