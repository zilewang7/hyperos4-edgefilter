#!/system/bin/sh
# EdgeFilter 核心 API：纯逻辑，只输出 JSON
# 用法: api.sh "action=status&since=12"
CFG=/data/adb/edgefilter/config
LOG=/data/adb/edgefilter/edgefilter.log
INFO=/data/adb/edgefilter/info
PIDF=/data/adb/edgefilter/edgefilter.pid
CTL=/data/adb/modules/edgefilter/ctl.sh

action=""; left=""; right=""; top=""; bottom=""
cleft=""; cright=""; ctop=""; cbottom=""
major=""; since="0"

for kv in $(echo "$1" | tr '&' ' '); do
    k=${kv%%=*}; v=${kv#*=}
    case "$k" in
        action)      action="$v" ;;
        left)        left="$v" ;;
        right)       right="$v" ;;
        top)         top="$v" ;;
        bottom)      bottom="$v" ;;
        coreLeft)    cleft="$v" ;;
        coreRight)   cright="$v" ;;
        coreTop)     ctop="$v" ;;
        coreBottom)  cbottom="$v" ;;
        major)       major="$v" ;;
        since)       since="$v" ;;
    esac
done

is_num() {
    case "$1" in
        ''|*[!0-9]*) return 1 ;;
    esac
    [ "$1" -le 10000000 ] && return 0
    return 1
}

read_cfg() {
    [ -f "$CFG" ] && . "$CFG"
    CFG_LEFT=${BAND_LEFT:-150}
    CFG_RIGHT=${BAND_RIGHT:-150}
    CFG_TOP=${BAND_TOP:-0}
    CFG_BOTTOM=${BAND_BOTTOM:-0}
    CFG_CLEFT=${CORE_LEFT:-0}
    CFG_CRIGHT=${CORE_RIGHT:-0}
    CFG_CTOP=${CORE_TOP:-0}
    CFG_CBOTTOM=${CORE_BOTTOM:-0}
    CFG_MAJOR=${MAJOR:-80}
    CFG_SCALE=${SCALE:-10}
}

status_json() {
    read_cfg
    running=false; pid=""
    if [ -f "$PIDF" ]; then
        pid=$(cat "$PIDF" 2>/dev/null)
        [ -n "$pid" ] && [ -d "/proc/$pid" ] && running=true
    fi

    panelx=0; panely=0; rot=90; devname=""
    # 常驻小文件，daemon 启动时写一次；没有就退回扫日志尾部
    if [ -f "$INFO" ]; then
        read px py rr dd < "$INFO" 2>/dev/null
        panelx=${px:-0}; panely=${py:-0}; rot=${rr:-90}; devname=$dd
    fi
    if [ "${panelx:-0}" -eq 0 ] && [ -f "$LOG" ]; then
        last=$(tail -n 200 "$LOG" 2>/dev/null | grep -a "panel 0\." | tail -n 1)
        if [ -n "$last" ]; then
            panelx=$(echo "$last" | sed -n 's/.*panel 0\.\.\([0-9]*\) x 0\.\.\([0-9]*\).*/\1/p')
            panely=$(echo "$last" | sed -n 's/.*panel 0\.\.\([0-9]*\) x 0\.\.\([0-9]*\).*/\2/p')
            rot=$(echo "$last" | sed -n 's/.*rot=\([0-9]*\).*/\1/p')
            devname=$(echo "$last" | sed -n 's/^\[edgefilter\] source \([^ ]*\) (\([^)]*\)).*/\2/p')
        fi
    fi

    total=0
    [ -f "$LOG" ] && total=$(wc -l < "$LOG" 2>/dev/null)
    [ -z "$total" ] && total=0

    printf '{"ok":true,"running":%s,"pid":"%s","device":"%s","panel":{"x":%s,"y":%s},"rot":%s,"log_lines":%s,"config":{"left":%s,"right":%s,"top":%s,"bottom":%s,"coreLeft":%s,"coreRight":%s,"coreTop":%s,"coreBottom":%s,"major":%s,"scale":%s}}' \
        "$running" "$pid" "$devname" "${panelx:-0}" "${panely:-0}" "${rot:-90}" "${total:-0}" \
        "$CFG_LEFT" "$CFG_RIGHT" "$CFG_TOP" "$CFG_BOTTOM" \
        "$CFG_CLEFT" "$CFG_CRIGHT" "$CFG_CTOP" "$CFG_CBOTTOM" \
        "$CFG_MAJOR" "$CFG_SCALE"
}

# 单进程 awk 一次性拼出 JSON。
# 旧实现每行 fork 一个 sed 做转义，40 行就要 ~3 秒，这是 web ui 卡顿的根源。
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
            if (since == 0)   { start = (n > 40) ? n - 40 : 0 }
            else if (n > max) { start = n - max }
            from = since + start
            if (from > total) from = total
            printf "{\"ok\":true,\"total\":%d,\"from\":%d,\"lines\":[", total, from
            for (i = start; i < n; i++) {
                s = buf[i]
                gsub(/\\/, "\\\\", s)
                gsub(/"/, "\\\"", s)
                gsub(/\t/, " ", s)
                if (i > start) printf ","
                printf "\"%s\"", s
            }
            printf "]}"
        }' "$LOG"
}

set_cfg() {
    for pair in "left:$left" "right:$right" "top:$top" "bottom:$bottom" \
                "coreLeft:$cleft" "coreRight:$cright" "coreTop:$ctop" "coreBottom:$cbottom" \
                "major:$major"; do
        name=${pair%%:*}; val=${pair#*:}
        [ -z "$val" ] && continue
        is_num "$val" || { printf '{"ok":false,"error":"bad %s"}' "$name"; exit 0; }
    done
    [ -n "$left" ]    && sed -i "s/^BAND_LEFT=.*/BAND_LEFT=$left/" "$CFG"
    [ -n "$right" ]   && sed -i "s/^BAND_RIGHT=.*/BAND_RIGHT=$right/" "$CFG"
    [ -n "$top" ]     && sed -i "s/^BAND_TOP=.*/BAND_TOP=$top/" "$CFG"
    [ -n "$bottom" ]  && sed -i "s/^BAND_BOTTOM=.*/BAND_BOTTOM=$bottom/" "$CFG"
    [ -n "$cleft" ]   && sed -i "s/^CORE_LEFT=.*/CORE_LEFT=$cleft/" "$CFG"
    [ -n "$cright" ]  && sed -i "s/^CORE_RIGHT=.*/CORE_RIGHT=$cright/" "$CFG"
    [ -n "$ctop" ]    && sed -i "s/^CORE_TOP=.*/CORE_TOP=$ctop/" "$CFG"
    [ -n "$cbottom" ] && sed -i "s/^CORE_BOTTOM=.*/CORE_BOTTOM=$cbottom/" "$CFG"
    [ -n "$major" ]   && sed -i "s/^MAJOR=.*/MAJOR=$major/" "$CFG"
    "$CTL" restart >/dev/null 2>&1
    printf '{"ok":true}'
}

case "$action" in
    status)  status_json ;;
    events)  events_json ;;
    set)     set_cfg ;;
    start)   "$CTL" start   >/dev/null 2>&1; status_json ;;
    stop)    "$CTL" stop    >/dev/null 2>&1; status_json ;;
    restart) "$CTL" restart >/dev/null 2>&1; status_json ;;
    *)       printf '{"ok":false,"error":"unknown action"}' ;;
esac
