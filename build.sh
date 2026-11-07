#!/usr/bin/env bash
# ============================================================================
# 构建可刷入的模块 zip
#   产物：dist/edgefilter-v<version>.zip
#   zip 根目录即模块根（module.prop 在根）
#
#   ./build.sh            构建（编译 daemon + 打包）
#   ./build.sh --check    只做一致性校验，不编译不打包（CI / 提交前用）
#   ./build.sh --no-cc    跳过编译，直接用已有的 module/bin/edgefilter
#
# 编译需要 Android NDK（arm64）。找不到时用 ANDROID_NDK_HOME 指定。
# ============================================================================
set -euo pipefail

cd "$(dirname "$0")"

VERSION=$(sed -n 's/^version=//p' module/module.prop)
VERSION=${VERSION#v}
VCODE=$(sed -n 's/^versionCode=//p' module/module.prop)
NAME="edgefilter-v${VERSION}"
OUT="dist/${NAME}.zip"
BIN="module/bin/edgefilter"

CHECK_ONLY=0
NO_CC=0
for a in "$@"; do
    case "$a" in
        --check) CHECK_ONLY=1 ;;
        --no-cc) NO_CC=1 ;;
        *) echo "未知参数: $a" >&2; exit 2 ;;
    esac
done

fail() { echo "✗ $*" >&2; exit 1; }
step() { echo "· $*"; }

# ============================================================ 一致性校验
step "校验模块文件"

for f in module.prop service.sh ctl.sh watch.sh web.sh api.sh action.sh \
         uninstall.sh config; do
    [ -f "module/$f" ] || fail "缺少 module/$f"
done
[ -f module/web/index.html ]       || fail "缺少 module/web/index.html"
[ -f module/web/cgi-bin/api ]      || fail "缺少 module/web/cgi-bin/api"
[ -f module/webroot/index.html ]   || fail "缺少 module/webroot/index.html"
[ -f src/edgefilter.c ]            || fail "缺少 src/edgefilter.c"

# shell 语法检查
for f in module/*.sh module/web/cgi-bin/api; do
    sh -n "$f" 2>/dev/null || fail "$f 语法错误"
done
step "  shell 语法 OK"

# webroot 与 web 的 index.html 必须一致（一个是 KSU WebUI，一个是 HTTP 控制台）
cmp -s module/web/index.html module/webroot/index.html \
    || fail "module/web/index.html 与 module/webroot/index.html 不一致"
step "  两份 index.html 一致"

# module.prop 必备字段
for k in id name version versionCode; do
    grep -q "^${k}=" module/module.prop || fail "module.prop 缺少 $k"
done
grep -q "^version=${VERSION}$" module/module.prop || fail "版本号解析异常"
step "  版本 v${VERSION} (versionCode ${VCODE})"

if [ "$CHECK_ONLY" = "1" ]; then
    echo
    echo "✓ 校验通过"
    exit 0
fi

# ============================================================ 编译 daemon
if [ "$NO_CC" = "0" ]; then
    step "编译 src/edgefilter.c"

    NDK="${ANDROID_NDK_HOME:-}"
    if [ -z "$NDK" ]; then
        for c in /usr/lib/android-sdk/ndk/* \
                 "${ANDROID_HOME:-/nonexistent}"/ndk/* \
                 "$HOME/Android/Sdk/ndk"/*; do
            [ -d "$c" ] && NDK="$c" && break
        done
    fi
    [ -n "$NDK" ] && [ -d "$NDK" ] || fail "找不到 Android NDK，设置 ANDROID_NDK_HOME 或用 --no-cc"

    CC=$(ls "$NDK"/toolchains/llvm/prebuilt/*/bin/aarch64-linux-android26-clang 2>/dev/null | head -1)
    [ -n "$CC" ] || fail "NDK 里找不到 aarch64-linux-android26-clang"

    mkdir -p module/bin
    "$CC" -static -O2 -Wall -Wextra -s -o "$BIN" src/edgefilter.c
    step "  $(basename "$CC") -> $BIN ($(stat -c%s "$BIN") bytes)"
else
    [ -f "$BIN" ] || fail "--no-cc 需要已存在的 $BIN"
    step "跳过编译，使用已有 $BIN"
fi

# ============================================================ 打包
step "打包"

rm -rf build dist
mkdir -p build dist

cp -a module/. build/
chmod 755 build/*.sh build/bin/edgefilter
chmod 644 build/module.prop build/config
chmod 644 build/web/index.html build/webroot/index.html
chmod 755 build/web/cgi-bin/api
chmod 755 build build/bin build/web build/webroot build/web/cgi-bin

python3 - "$OUT" <<'PY'
import sys, os, zipfile

out = sys.argv[1]
root = "build"

EXEC = {".sh", "api"}

with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for dirpath, _, files in os.walk(root):
        for fn in sorted(files):
            full = os.path.join(dirpath, fn)
            rel = os.path.relpath(full, root)
            exe = fn.endswith(".sh") or fn == "api" or fn == "edgefilter"
            mode = 0o755 if exe else 0o644
            info = zipfile.ZipInfo(rel, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = mode << 16
            with open(full, "rb") as f:
                z.writestr(info, f.read())
print(f"  {out}  ({os.path.getsize(out)} bytes)")
PY

echo
unzip -l "$OUT" | sed -n '1,30p'
echo
echo "版本: v${VERSION}"
echo "产物: $OUT"
