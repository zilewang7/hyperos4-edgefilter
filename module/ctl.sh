#!/system/bin/sh
# edgefilter 控制脚本: start | stop | restart | heal | status | log
MODDIR=$(cd "$(dirname "$0")" && pwd)
DATA=/data/adb/edgefilter
CFG=$DATA/config
PIDF=$DATA/edgefilter.pid
READY=$DATA/ready
LOG=$DATA/edgefilter.log
BIN="$MODDIR/bin/edgefilter"
mkdir -p "$DATA"
[ -f "$CFG" ] && . "$CFG"

# start/stop/restart 单飞。toybox flock 只能锁已打开的 fd，而 Android 的 sh 不会
# 把它 exec 给子进程，所以这里用 busybox 的 `flock FILE PROG` 形式重新执行自己。
BB=/data/adb/ksu/bin/busybox
[ -x "$BB" ] || BB=$(command -v busybox 2>/dev/null)
if [ -n "$BB" ] && [ -z "$EDGEFILTER_LOCKED" ]; then
    EDGEFILTER_LOCKED=1
    export EDGEFILTER_LOCKED
    exec "$BB" flock "$DATA/ctl.lock" "$0" "$@"
fi

read_pid() {
    [ -f "$PIDF" ] && cat "$PIDF" 2>/dev/null
}

is_running() {
    pid=$(read_pid)
    [ -n "$pid" ] && [ -r "/proc/$pid/comm" ] || return 1
    [ "$(cat "/proc/$pid/comm" 2>/dev/null)" = "edgefilter" ]
}

is_ready() {
    is_running && [ "$(cat "$READY" 2>/dev/null)" = "1" ]
}

do_start() {
    if is_ready; then
        echo "already running (pid $(read_pid))"
        return 0
    fi
    if is_running; then
        echo "already starting (pid $(read_pid))"
        return 0
    fi
    rm -f "$PIDF" "$READY"
    if [ -f "$LOG" ] && [ "$(wc -c < "$LOG" 2>/dev/null || echo 0)" -gt 1048576 ]; then
        mv -f "$LOG" "$LOG.1"
    fi
    nohup "$BIN" --name "${TOUCH_NAME:-goodix_ts}" \
        --left "${BAND_LEFT:-150}" --right "${BAND_RIGHT:-150}" \
        --top "${BAND_TOP:-0}" --bottom "${BAND_BOTTOM:-0}" \
        --major "${MAJOR:-40}" \
        --core-left "${CORE_LEFT:-40}" --core-right "${CORE_RIGHT:-40}" \
        --core-top "${CORE_TOP:-0}" --core-bottom "${CORE_BOTTOM:-0}" \
        --core-slop "${CORE_SLOP:-12}" --core-hold-ms "${CORE_HOLD_MS:-200}" \
        --scale "${SCALE:-10}" --rot "${ROT:-auto}" \
        >> "$LOG" 2>&1 &
    pid=$!
    echo "$pid" > "$PIDF.tmp"
    mv -f "$PIDF.tmp" "$PIDF"
    sleep 1
    if is_ready; then
        echo "started (pid $pid)"
        tail -n 3 "$LOG"
    else
        echo "start FAILED, native touch restored; log:"
        tail -n 10 "$LOG"
        return 1
    fi
}

do_stop() {
    if ! is_running; then
        rm -f "$PIDF" "$READY"
        echo "not running"
        return 0
    fi
    pid=$(read_pid)
    kill "$pid" 2>/dev/null
    for i in 1 2 3 4 5; do
        is_running || break
        sleep 0.4
    done
    if is_running; then kill -9 "$pid" 2>/dev/null; fi
    rm -f "$PIDF" "$READY"
    echo "stopped; native touch restored"
}

case "$1" in
    start)   do_start ;;
    stop)    do_stop ;;
    restart) do_stop; sleep 0.2; do_start ;;
    # 面板卡在手势模式时，交回 event5 几秒比任何重抓都有效
    heal)    do_stop; sleep 3; do_start ;;
    status)
        if is_ready; then echo "running (pid $(read_pid), ready)"
        elif is_running; then echo "starting (pid $(read_pid), not ready)"
        else echo "stopped"; fi
        ;;
    log) tail -n "${2:-30}" "$LOG" ;;
    *) echo "usage: $0 {start|stop|restart|heal|status|log [n]}"; exit 2 ;;
esac
