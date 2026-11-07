#!/system/bin/sh
# EdgeFilter Web 控制台启停
WEB=/data/adb/edgefilter/web
BB=/data/adb/ksu/bin/busybox
CFG=/data/adb/edgefilter/config
[ -f "$CFG" ] && . "$CFG"
PORT=${WEB_PORT:-8089}

running() {
    # [h]ttpd keeps grep from matching its own command line
    ps -A -o ARGS 2>/dev/null | grep -q "[h]ttpd -p $PORT"
}

case "$1" in
    start)
        if running; then echo "web already running on $PORT"; exit 0; fi
        $BB httpd -p "$PORT" -h "$WEB"
        sleep 0.5
        if running; then echo "web started on $PORT"; else echo "web start FAILED"; exit 1; fi
        ;;
    stop)
        pkill -f "httpd -p $PORT" 2>/dev/null
        echo "web stopped"
        ;;
    status)
        if running; then echo "web running on $PORT"; else echo "web stopped"; fi
        ;;
    *)
        echo "usage: $0 {start|stop|status}"; exit 2 ;;
esac
