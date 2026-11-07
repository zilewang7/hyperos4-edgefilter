#!/system/bin/sh
# edgefilter 控制脚本: start | stop | restart | status | log
MODDIR=$(cd "$(dirname "$0")" && pwd)
CFG=/data/adb/edgefilter/config
PIDF=/data/adb/edgefilter/edgefilter.pid
LOG=/data/adb/edgefilter/edgefilter.log
BIN="$MODDIR/bin/edgefilter"

[ -f "$CFG" ] && . "$CFG"

is_running() {
    [ -f "$PIDF" ] || return 1
    pid=$(cat "$PIDF" 2>/dev/null)
    [ -n "$pid" ] && [ -d "/proc/$pid" ] && return 0
    return 1
}

do_start() {
    if is_running; then
        echo "already running (pid $(cat $PIDF))"
        return 0
    fi
    mkdir -p /data/adb/edgefilter
    if [ -f "$LOG" ] && [ "$(wc -c < "$LOG" 2>/dev/null || echo 0)" -gt 1048576 ]; then mv -f "$LOG" "$LOG.1"; fi
    nohup "$BIN" --name "${TOUCH_NAME:-goodix_ts}" \
        --left "${BAND_LEFT:-150}" --right "${BAND_RIGHT:-150}" \
        --top "${BAND_TOP:-0}" --bottom "${BAND_BOTTOM:-0}" \
        --core-left "${CORE_LEFT:-0}" --core-right "${CORE_RIGHT:-0}" \
        --core-top "${CORE_TOP:-0}" --core-bottom "${CORE_BOTTOM:-0}" \
        --core-slop "${CORE_SLOP:-12}" \
        --scale "${SCALE:-10}" --rot "${ROT:-auto}" \
        ${MAJOR:+--major $MAJOR} \
        ${VERBOSE:+-v} >> "$LOG" 2>&1 &
    echo $! > "$PIDF"
    sleep 1
    if is_running; then
        echo "started (pid $(cat $PIDF))"
        tail -n 3 "$LOG"
    else
        echo "start FAILED, log:"
        tail -n 10 "$LOG"
        return 1
    fi
}

do_stop() {
    if ! is_running; then
        rm -f "$PIDF"
        echo "not running"
        return 0
    fi
    pid=$(cat "$PIDF")
    kill "$pid" 2>/dev/null
    for i in 1 2 3 4 5; do
        [ -d "/proc/$pid" ] || break
        sleep 0.4
    done
    [ -d "/proc/$pid" ] && kill -9 "$pid" 2>/dev/null
    rm -f "$PIDF"
    echo "stopped"
}

case "$1" in
    start)   do_start ;;
    stop)    do_stop ;;
    restart) do_stop; sleep 0.5; do_start ;;
    status)
        if is_running; then
            echo "running (pid $(cat $PIDF))"
        else
            echo "stopped"
        fi
        ;;
    log) tail -n "${2:-30}" "$LOG" ;;
    *) echo "usage: $0 {start|stop|restart|status|log [n]}"; exit 2 ;;
esac
