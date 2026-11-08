#!/system/bin/sh
# EdgeFilter API：KSU exec 与 HTTP CGI 共用，只输出 JSON。
CFG=/data/adb/edgefilter/config
LOG=/data/adb/edgefilter/edgefilter.log
INFO=/data/adb/edgefilter/info
PIDF=/data/adb/edgefilter/edgefilter.pid
READY=/data/adb/edgefilter/ready
CTL=/data/adb/modules/edgefilter/ctl.sh

action=""; left=""; right=""; top=""; bottom=""; major=""; since="0"
for kv in $(echo "$1" | tr '&' ' '); do
    k=${kv%%=*}; v=${kv#*=}
    case "$k" in
        action) action="$v" ;; left) left="$v" ;; right) right="$v" ;;
        top) top="$v" ;; bottom) bottom="$v" ;; major) major="$v" ;;
        since) since="$v" ;;
    esac
done

is_num() {
    case "$1" in ''|*[!0-9]*) return 1 ;; esac
    [ "$1" -le 10000000 ]
}

read_cfg() {
    [ -f "$CFG" ] && . "$CFG"
    CFG_LEFT=${BAND_LEFT:-150}; CFG_RIGHT=${BAND_RIGHT:-150}
    CFG_TOP=${BAND_TOP:-0}; CFG_BOTTOM=${BAND_BOTTOM:-0}
    CFG_MAJOR=${MAJOR:-40}; CFG_SCALE=${SCALE:-10}
}

status_json() {
    read_cfg
    running=false; pid=""
    if [ -f "$PIDF" ]; then
        pid=$(cat "$PIDF" 2>/dev/null)
        if [ -n "$pid" ] && [ "$(cat "/proc/$pid/comm" 2>/dev/null)" = "edgefilter" ] \
            && [ "$(cat "$READY" 2>/dev/null)" = "1" ]; then running=true; fi
    fi

    panelx=0; panely=0; rot=90; devname=""
    if [ -f "$INFO" ]; then
        read px py rr dd < "$INFO" 2>/dev/null
        panelx=${px:-0}; panely=${py:-0}; rot=${rr:-90}; devname=$dd
    fi
    total=0
    [ -f "$LOG" ] && total=$(wc -l < "$LOG" 2>/dev/null)
    [ -z "$total" ] && total=0

    printf '{"ok":true,"running":%s,"pid":"%s","device":"%s","panel":{"x":%s,"y":%s},"rot":%s,"log_lines":%s,"config":{"left":%s,"right":%s,"top":%s,"bottom":%s,"major":%s,"scale":%s}}' \
        "$running" "$pid" "$devname" "${panelx:-0}" "${panely:-0}" "${rot:-90}" "${total:-0}" \
        "$CFG_LEFT" "$CFG_RIGHT" "$CFG_TOP" "$CFG_BOTTOM" "$CFG_MAJOR" "$CFG_SCALE"
}

events_json() {
    is_num "$since" || since=0
    if [ ! -f "$LOG" ]; then
        printf '{"ok":true,"total":0,"from":0,"lines":[]}'
        return
    fi
    awk -v since="$since" -v max=120 '
        { if (NR > since) buf[n++] = $0 }
        END {
            total = NR
            start = 0
            if (since == 0) start = (n > 40) ? n - 40 : 0
            else if (n > max) start = n - max
            from = since + start
            if (from > total) from = total
            printf "{\"ok\":true,\"total\":%d,\"from\":%d,\"lines\":[", total, from
            for (i = start; i < n; i++) {
                s = buf[i]
                gsub(/\\/, "\\\\", s); gsub(/"/, "\\\"", s); gsub(/\t/, " ", s)
                if (i > start) printf ","
                printf "\"%s\"", s
            }
            printf "]}"
        }' "$LOG"
}

set_cfg() {
    for pair in "left:$left" "right:$right" "top:$top" "bottom:$bottom" "major:$major"; do
        name=${pair%%:*}; val=${pair#*:}
        [ -z "$val" ] && continue
        is_num "$val" || { printf '{"ok":false,"error":"bad %s"}' "$name"; return; }
    done
    read_cfg
    [ -n "$left" ] && CFG_LEFT=$left; [ -n "$right" ] && CFG_RIGHT=$right
    [ -n "$top" ] && CFG_TOP=$top; [ -n "$bottom" ] && CFG_BOTTOM=$bottom
    [ -n "$major" ] && CFG_MAJOR=$major

    tmp="$CFG.tmp.$$"
    sed -e "s/^BAND_LEFT=.*/BAND_LEFT=$CFG_LEFT/" \
        -e "s/^BAND_RIGHT=.*/BAND_RIGHT=$CFG_RIGHT/" \
        -e "s/^BAND_TOP=.*/BAND_TOP=$CFG_TOP/" \
        -e "s/^BAND_BOTTOM=.*/BAND_BOTTOM=$CFG_BOTTOM/" \
        -e "s/^MAJOR=.*/MAJOR=$CFG_MAJOR/" "$CFG" > "$tmp" || {
        rm -f "$tmp"; printf '{"ok":false,"error":"config write"}'; return;
    }
    chmod 644 "$tmp"
    mv -f "$tmp" "$CFG" || { rm -f "$tmp"; printf '{"ok":false,"error":"config rename"}'; return; }
    if "$CTL" restart >/dev/null 2>&1; then
        printf '{"ok":true}'
    else
        printf '{"ok":false,"error":"restart failed; native touch restored"}'
    fi
}

# 配置写入串行化。toybox flock 无法锁 sh 打开的 fd，改用 busybox `flock FILE PROG`。
BB=/data/adb/ksu/bin/busybox
[ -x "$BB" ] || BB=$(command -v busybox 2>/dev/null)
if [ "$action" = "set" ] && [ -n "$BB" ] && [ -z "$EDGEFILTER_CFG_LOCKED" ]; then
    EDGEFILTER_CFG_LOCKED=1
    export EDGEFILTER_CFG_LOCKED
    exec "$BB" flock "$CFG.lock" "$0" "$1"
fi

case "$action" in
    status) status_json ;;
    events) events_json ;;
    set) set_cfg ;;
    start) "$CTL" start >/dev/null 2>&1; status_json ;;
    stop) "$CTL" stop >/dev/null 2>&1; status_json ;;
    *) printf '{"ok":false,"error":"unknown action"}' ;;
esac
