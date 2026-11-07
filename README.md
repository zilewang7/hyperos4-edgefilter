# EdgeFilter 边缘防误触

[![build](https://github.com/zilewang7/hyperos4-edgefilter/actions/workflows/build.yml/badge.svg)](https://github.com/zilewang7/hyperos4-edgefilter/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/zilewang7/hyperos4-edgefilter)](https://github.com/zilewang7/hyperos4-edgefilter/releases/latest)

KernelSU 模块。在用户态抓取触摸屏，丢掉落在边缘带里的误触，再经 uinput 重新注入 ——
给**触控固件不支持原生 edge suppression** 的机型补上边缘防误触。

手里平板（Xiaomi Pad 7 Ultra / `jinghu`）握持时，虎口和拇指贴着边框，很容易误触。
小米框架层其实有 `EdgeSuppressionManager`，但这台机器的 `ro.vendor.touchfeature.type`
没有 edge mode 位，就算强行改了属性让框架实例化出来、也真的算出了抑制矩形，
**goodix 的固件和 HAL 根本不执行** —— 所以只能自己做。

| 能做 | 不能做 |
|---|---|
| 拦掉贴着边框的拇指 / 手掌按压 | 拦掉笔（`event6/7` 独立设备，不碰） |
| 保留边缘侧滑返回、上滑回桌面 | 需要改内核或刷分区（纯用户态，root 即可） |
| 转屏后自动重新映射 | 免解锁越狱的 KSU 重启后要手动拉起一次 |
| WebUI 可视化调参 + 实时判定预览 | |

## 原理

```
goodix_ts (event5)
      │  EVIOCGRAB 独占
      ▼
  edgefilter  ── 判定 ──►  uinput: goodix_ts-filtered ──► InputReader ──► 应用
```

1. `EVIOCGRAB` 独占真实触摸屏，Android 收不到原始事件。
2. 建一个 uinput 虚拟多点触控设备（protocol B），**能力位完全复制源设备**。
3. 每个触点只在**起手那一帧**判定一次：落在边缘带里就整段丢弃，否则原样转发。

虚拟设备不是"另造一个触摸屏"，而是**透明代理**：源设备的全部按键和 ABS 能力位
都复制过去，非触控事件（双击亮屏的 `KEY_WAKEUP`、`ABS_X/ABS_Y`…）一律原样透传。
Android 侧看到的 `Classes` 和原设备一致（`KEYBOARD | TOUCH | TOUCH_MT`）。

> 这一步很关键。早期版本只声明了 `BTN_TOUCH` / `BTN_TOOL_FINGER`，把 `KEY_WAKEUP`
> 吞掉了 —— 结果是双击亮屏失效，触摸 IC 卡在手势模式里出不来，表现为"屏幕亮着但
> 触摸完全没反应"。

## 两层边缘带

```
        ┌───────────────────────────────┐
        │██│                         │██│   核心带 CORE_* → 原地点击取消，滑动放行
        │▓▓│                         │▓▓│   外层带 BAND_* → 面积 ≥ MAJOR 才拦
        │  │                         │  │
        │  │        可触摸区          │  │
        │  │                         │  │
        │▓▓│                         │▓▓│
        │██│                         │██│
        └───────────────────────────────┘
```

判定顺序：**先看核心带，再看外层带**。

- **核心带内**：DOWN 立刻转发（手势零延迟）；如果一直没怎么动就抬手，判定为原地误触，
  发 `SYN_DROPPED` 把这次触摸取消掉，应用收到 `ACTION_CANCEL`，不会触发点击；
  移动超过 `CORE_SLOP` 则判定为手势，正常放行。
- **外层带内（核心带之外）**：接触面积 ≥ `MAJOR` 才拦。
- 其余：直接放行。

### 为什么核心带不能"扣住 DOWN"

早期实现是：核心带内的触点先把 DOWN 扣住，等确认是滑动再补发一个合成 DOWN。
系统的返回手势识别拿到的起点**迟到且跳变**，侧滑返回和上滑回桌面成功率只有一半左右。

现在改成「立刻转发 + 事后取消」，手势拿到的是完全真实的触摸流。

### `MAJOR` 怎么定

这台机器实测：

| | 面积 `ABS_MT_TOUCH_MAJOR` |
|---|---|
| 指尖点击 / 侧滑起手 | 12 ~ 36 |
| 拇指贴着边框握持 | 40 ~ 76 |

所以 `MAJOR=40` 正好：握持边框的拇指被拦，指尖操作照常。
调到 76 以上握持时会漏；设成 `0` 则外层带内全拦，边缘手势全废。

## 兼容性

已验证：

- Xiaomi Pad 7 Ultra (`25032RP42C`)，HyperOS `OS4.0.0.16.XOXCNXM`，Android 17 (SDK 37)，arm64-v8a
- KernelSU v3.3.1（免解锁越狱的 CVE 临时 root），bootloader 锁定、AVB 完好

原理上只要是 `evdev` 多点触控设备（`ABS_MT_SLOT` + `ABS_MT_TRACKING_ID`）都能用，
`TOUCH_NAME` 换成自己的设备名即可。没在别的机型上验证过。

## 安装

1. 从 [Releases](https://github.com/zilewang7/hyperos4-edgefilter/releases/latest) 下载 `edgefilter-v*.zip`。
2. 在 KernelSU 管理器里「安装模块」并重启。
3. 打开模块卡片的「操作」按钮，或访问 WebUI 确认状态为「运行中」。

命令行装法：

```bash
adb push edgefilter-v0.8.zip /data/local/tmp/
adb shell 'su -c "ksud module install /data/local/tmp/edgefilter-v0.8.zip"'
```

> **免解锁越狱（CVE 临时 root）注意**：KSU 的 `service.sh` 只在开机阶段执行，
> 而临时 root 是开机**之后**才注入的，所以重启后模块不会自启。重挂 root 后手动拉起一次：
>
> ```bash
> su -c sh /data/adb/modules/edgefilter/service.sh
> ```
>
> 或者点模块卡片的「操作」按钮，`action.sh` 里是同一套逻辑。

## 配置

`/data/adb/edgefilter/config`（改完 `ctl.sh restart` 生效，WebUI 里改会自动重启）：

| 键 | 默认 | 含义 |
|---|---|---|
| `BAND_LEFT` / `BAND_RIGHT` | 150 | 外层左右边缘带宽度（逻辑像素，150 ≈ 1cm） |
| `BAND_TOP` / `BAND_BOTTOM` | 0 | 外层上下边缘带，0 = 不拦 |
| `CORE_LEFT` / `CORE_RIGHT` | 40 | 核心带宽度，落点在这里的**原地点击**会被取消；0 = 该边没有核心带 |
| `CORE_TOP` / `CORE_BOTTOM` | 0 | 核心带上下 |
| `CORE_SLOP` | 12 | 核心带内移动超过这么多 px 就当作手势放行；0 = 核心带内一律拦截 |
| `SCALE` | 10 | 逻辑像素 → 原始触控单位换算（本机 1px = 10 单位） |
| `MAJOR` | 40 | 外层带的面积阈值；0 = 外层带内全拦 |
| `TOUCH_NAME` | `goodix_ts` | 按设备名找 event 节点，避免重启后编号漂移 |
| `ROT` | `auto` | 自动读旋转并跟随转屏；也可写死 `0/90/180/270` |
| `WEB_PORT` | 8089 | HTTP 控制台端口 |
| `VERBOSE` | 空 | 设为 1 打印每个触点判定 |

核心带必须 ≤ 对应的外层带（WebUI 里会自动 clamp）。

## 控制

```bash
su -c /data/adb/modules/edgefilter/ctl.sh start|stop|restart|status|log
```

- `stop` 会释放 `EVIOCGRAB`，触控立刻恢复原生行为。
- 进程异常退出 / 被杀 → fd 关闭 → grab 自动释放，**不会把触控锁死**。
- 写 uinput 失败（设备被系统摘掉）会自动重建；重建失败则释放 grab 退回原生。

### WebUI

两个入口，同一套 UI，都调用同一个 `api.sh`：

| 入口 | 用在哪 | 加载的文件 | 通信 |
|---|---|---|---|
| KSU 模块卡片「打开」 | 平板上 | `module/webroot/index.html` | `ksu.exec()`，不占端口 |
| `http://<ip>:8089/` | 电脑上 | `/data/adb/edgefilter/web/index.html` | busybox httpd + CGI |

HTTP 那份是 `service.sh` 开机时从模块目录 `cp` 过去的副本，改了模块里的 `index.html`
要等下次开机（或手动跑一次 `service.sh`）才会同步。

浏览器版可以设访问令牌：往 `/data/adb/edgefilter/web.token` 写一行任意字符串即可。

## 亮灭屏与转屏

`watch.sh` 常驻看护：

| 事件 | 信号 | daemon 动作 |
|---|---|---|
| 亮屏 `screen_toggled: 1` | `SIGHUP` | 重新 grab + 新建虚拟设备 + 清空触点状态 + 重读旋转 |
| 灭屏 `screen_toggled: 0` | `SIGUSR1` | 释放 grab + 删除虚拟设备，手势唤醒完全交还原生路径 |
| 旋转变化 | `SIGUSR2` | 重读旋转，变了就更新映射并重写 `info` |

灭屏时面板 100% 交还原生路径，双击唤醒、AOD 手势、触摸 IC 的 doze 状态机都不受干扰。

转屏检测放在 `watch.sh` 里做（`dumpsys input` 一次约 45ms），只有值真的变了才通知 daemon，
不把开销带进触摸事件循环。两路来源：

- `configuration_changed` 事件（快，但**实测不可靠**：经常早于显示真正转过来，读到旧值）
- **每 2 秒一次的轮询**（真正保证正确性，最坏延迟约 2 秒）

```
[edgefilter] rotation changed 0 -> 90
```

## 排查

**边缘手势（侧滑返回 / 上滑回桌面）不灵**

按顺序查：

1. `MAJOR` 是否在 40~80 之间。低于指尖面积（12~36）会连手指一起拦。
2. 核心带是否比外层带还宽。核心带只该盖住"贴着边框那一小条"。
3. `CORE_SLOP` 是否为 0 —— 那等于核心带内一律拦截，手势必废。
4. 临时全关验证：`CORE_LEFT=0 CORE_RIGHT=0 CORE_TOP=0 CORE_BOTTOM=0`，重启 daemon。

**触摸完全没反应**

先看状态，再重启：

```bash
su -c /data/adb/modules/edgefilter/ctl.sh status
su -c /data/adb/modules/edgefilter/ctl.sh restart
```

实在不行 `ctl.sh stop` 立刻恢复原生触控，然后看 `/data/adb/edgefilter/edgefilter.log`。

**WebUI 打开是空白 / 不刷新**

刷新一次页面。还不行就看 `/data/adb/edgefilter/web/index.html` 是不是旧版本
（开机副本，见上文「WebUI」）。

## 已知限制

- 需要 root。抓住设备后，`getevent` 之类的原始读取读不到数据（被独占）。
- 触控笔（`event6/7`）不受影响，仍走原设备 —— 也就意味着**笔的误触不会拦**。
- 灭屏期间不做过滤（不需要，也不会和手势路径打架）；亮屏瞬间有一次极短的 grab 空窗。
- 只在起手那一帧判定。手掌先落在中间再滑到边缘不会被拦。

## 从源码构建

需要 Android NDK（arm64）和 Python 3：

```bash
./build.sh            # 编译 + 打包 → dist/edgefilter-v<version>.zip
./build.sh --check    # 只做一致性校验
./build.sh --no-cc    # 跳过编译，直接用 module/bin/edgefilter
```

NDK 路径依次从 `ANDROID_NDK_HOME`、`$ANDROID_HOME/ndk/*`、`~/Android/Sdk/ndk/*`
自动查找。编译用 `aarch64-linux-android26-clang -static -O2`。

推 `v*` tag 会自动构建并发布 Release（见 `.github/workflows/release.yml`）。

## 项目结构

```
.
├── .github/workflows/       # build（每次 push）/ release（v* tag）
├── build.sh                 # 校验 + 编译 + 打包
├── docs/                    # 截图
├── src/
│   └── edgefilter.c         # daemon 源码（单文件，静态编译）
└── module/                  # 模块载荷，zip 根目录即此目录
    ├── module.prop
    ├── service.sh           # 开机启动：同步 web 文件 + 拉起服务
    ├── ctl.sh               # start/stop/restart/status/log
    ├── watch.sh             # 亮灭屏 + 转屏看护
    ├── web.sh               # busybox httpd 启停
    ├── api.sh               # 业务逻辑，输出 JSON（WebUI / CGI 共用）
    ├── action.sh            # 模块卡片「操作」按钮
    ├── uninstall.sh
    ├── config               # 默认配置
    ├── bin/edgefilter       # aarch64 静态二进制（build.sh 生成）
    ├── web/                 # 浏览器版控制台（httpd 根目录）
    │   ├── index.html
    │   └── cgi-bin/api
    └── webroot/index.html   # KernelSU WebUI 入口
```

## License

[MIT](LICENSE) © 2026 zilewang7

---

## English

A KernelSU module that adds edge touch rejection to devices whose touch firmware does not
implement native edge suppression.

It grabs the real touchscreen with `EVIOCGRAB`, re-emits everything through a uinput
multi-touch device that mirrors the source device's full capability set, and drops contacts
whose **first** point falls inside a configurable edge band.

Two nested bands: an **outer band** that only rejects contacts with area ≥ `MAJOR`
(a thumb resting on the bezel), and an inner **core band** where a stationary tap is
cancelled with `SYN_DROPPED` while swipes pass through untouched — so edge back gestures
keep working.

A bundled WebUI (KernelSU module card, or `http://<ip>:8089/`) shows live decisions on a
screen preview and lets you tune every value.

Requires root. Tested on Xiaomi Pad 7 Ultra (`jinghu`) / HyperOS 4 / Android 17 / KernelSU.
