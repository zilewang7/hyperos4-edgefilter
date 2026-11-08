# EdgeFilter 边缘防误触

[![build](https://github.com/zilewang7/hyperos4-edgefilter/actions/workflows/build.yml/badge.svg)](https://github.com/zilewang7/hyperos4-edgefilter/releases/latest)

KernelSU 模块。在用户态抓取触摸屏，把**贴边握持**的整段触点丢掉，其余触点原样透传 ——
给**触控固件不支持原生 edge suppression** 的机型补上边缘防误触。

手里平板（Xiaomi Pad 7 Ultra / `jinghu`）握持时，虎口和拇指贴着边框，很容易误触。
小米框架层其实有 `EdgeSuppressionManager`，但这台机器的 `ro.vendor.touchfeature.type`
没有 edge mode 位，就算强行改了属性让框架实例化出来、也真的算出了抑制矩形，
**goodix 的固件和 HAL 根本不执行** —— 所以只能自己做。

| 能做 | 不能做 |
|---|---|
| 丢掉贴着边框的拇指 / 手掌按压 | 拦掉笔（`event6/7` 独立设备，不碰） |
| 保留边缘侧滑返回、上滑回桌面 | 需要改内核或刷分区（纯用户态，root 即可） |
| 转屏后自动重新映射 | 免解锁越狱的 KSU 重启后要手动拉起一次 |
| WebUI 可视化调参 + 实时判定预览 | |

## 设计原则：less is more

v0.9 是重写，不是调参。上一版为了做到「贴边小点击取消、但滑动照常放行」，引入了
延迟判定、合成 `DOWN`、抬手 `SYN_DROPPED` 取消。这些机制**必然会破坏触控流**：

- 合成 `DOWN` 让落点和手势起点跳变 → 点击漂移
- `SYN_DROPPED` 是**整台设备**的事件丢失通知，不是单指取消 → 幽灵按住、多指失步
- 息屏销毁/重建 uinput 设备 → 与触摸 IC 的 doze 状态机抢状态
- 非阻塞整帧写入不处理短写/`EAGAIN` → 丢掉 `TRACKING_ID=-1` 后触点永久按住

新版本只保留一条规则：

> **每个触点在第一个 `SYN_REPORT` 判定一次：信息完整的边缘握持整段丢弃，其余整段原样透传。**

没有延迟、没有合成事件、没有事后取消。代价是「贴边原地点击取消、滑动放行」这个功能被移除 ——
它无法在不破坏透明性的前提下可靠实现。

## 原理

```
goodix_ts (event5)
      │  EVIOCGRAB 独占
      ▼
  edgefilter  ── 首帧判定 ──►  uinput: goodix_ts-filtered ──► InputReader ──► 应用
```

1. `EVIOCGRAB` 独占真实触摸屏，Android 收不到原始事件。
2. 建一个 uinput 虚拟多点触控设备（protocol B），**能力位完全复制源设备**。
3. 每个触点在**起手那一帧**判定一次：落在边缘带里且面积达阈值就整段丢弃，否则原样转发。

### 硬性不变量

这些是代码里刻意维持、并由 `tests/protocol_test.c` 断言的性质：

| 不变量 | 意义 |
|---|---|
| 未被过滤的帧逐事件、逐字节不变 | 时间戳、顺序、坐标都不重排 |
| `BTN_TOUCH` / `BTN_TOOL_FINGER` 只由**实际输出的触点**决定 | 不盲传源设备的全局按键 |
| 被丢弃的触点：零 `DOWN`、零坐标、零 legacy 轴 | 不泄漏任何痕迹 |
| 一个 `PASS` 触点恰好一个 `DOWN`、一个 `UP` | 不重复、不丢失 |
| 首帧缺坐标或面积 → **放行** | 绝不用旧值猜、绝不延迟 `DOWN` |
| 写入失败 / 源 `SYN_DROPPED` / 帧溢出 → **退出并恢复原生** | 绝不带着失真状态继续独占面板 |
| `grabbed` ⇒ 虚拟设备已就绪 | 不会出现「抓住面板但没输出」 |

启动时还会检查原生触点状态：**如果手指正按着，拒绝启动并保持原生触控**，等抬起后再启动。
`EVIOCGRAB` 不会替其他客户端取消已经收到的 `DOWN`，从半段手势开始转发会造成残留。

### 为什么不再处理亮灭屏

虚拟设备在亮灭屏之间**保持不变**，也不释放 grab。源设备的全部按键能力（包括双击唤醒的
`KEY_WAKEUP`、`KEY_POWER`）都复制并原样透传，Android 看到的事件流与没有模块时一致。

上一版在息屏时释放 grab、销毁 uinput，亮屏时重建。反复拆装输入设备会扰动
InputReader 与触摸 IC 的模式状态，是「亮屏后触摸无响应」的可疑来源之一。

## 边缘带

```
        ┌───────────────────────────────┐
        │▓▓│                         │▓▓│   边缘带 BAND_* → 面积 ≥ MAJOR 才丢
        │▓▓│                         │▓▓│
        │  │                         │  │
        │  │        可触摸区          │  │
        │  │                         │  │
        │▓▓│                         │▓▓│
        │▓▓│                         │▓▓│
        └───────────────────────────────┘
```

- 触点起手帧落在边缘带内，且 `ABS_MT_TOUCH_MAJOR ≥ MAJOR` → 整段丢弃。
- 其余 → 整段原样透传。

### `MAJOR` 怎么定

这台机器实测：

| | 面积 `ABS_MT_TOUCH_MAJOR` |
|---|---|
| 指尖点击 / 侧滑起手 | 12 ~ 36 |
| 拇指贴着边框握持 | 40 ~ 76 |

所以 `MAJOR=40` 正好：握持边框的拇指被拦，指尖操作照常。
调到 76 以上握持时会漏；设成 `0` 则边缘带内全拦，边缘手势全废。

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
adb push edgefilter-v0.9.zip /data/local/tmp/
adb shell 'su -c "ksud module install /data/local/tmp/edgefilter-v0.9.zip"'
```

> **免解锁越狱（CVE 临时 root）注意**：KSU 的 `service.sh` 只在开机阶段执行，
> 而临时 root 是开机**之后**才注入的，所以重启后模块不会自启。重挂 root 后手动拉起一次：
>
> ```bash
> su -c sh /data/adb/modules/edgefilter/service.sh
> ```
>
> 或者点模块卡片的「操作」按钮，`action.sh` 调的是同一个入口。

## 配置

`/data/adb/edgefilter/config`（改完 `ctl.sh restart` 生效，WebUI 里改会自动重启）：

| 键 | 默认 | 含义 |
|---|---|---|
| `BAND_LEFT` / `BAND_RIGHT` | 150 | 左右边缘带宽度（逻辑像素，150 ≈ 1cm） |
| `BAND_TOP` / `BAND_BOTTOM` | 0 | 上下边缘带，0 = 不拦 |
| `MAJOR` | 40 | 边缘带内的面积阈值；0 = 边缘带内全拦 |
| `SCALE` | 10 | 逻辑像素 → 原始触控单位换算（本机 1px = 10 单位） |
| `TOUCH_NAME` | `goodix_ts` | 按设备名找 event 节点，避免重启后编号漂移 |
| `ROT` | `auto` | 自动读旋转并跟随转屏；也可写死 `0/90/180/270` |
| `WEB_PORT` | 8089 | HTTP 控制台端口 |

## 控制

```bash
su -c /data/adb/modules/edgefilter/ctl.sh start|stop|restart|status|log
```

- `stop` 会释放 `EVIOCGRAB`，触控立刻恢复原生行为。
- 进程异常退出 / 被杀 → fd 关闭 → grab 自动释放，**不会把触控锁死**。
- 任何输出错误 → 主动退出，而不是继续过滤。
- `start/stop/restart` 之间有文件锁，WebUI 连点也不会并发启动多个实例。
- daemon 自己也持有单实例锁，绕开脚本手动启动也不会出现第二个代理。

### WebUI

两个入口，同一套 UI，都调用同一个 `api.sh`：

| 入口 | 用在哪 | 加载的文件 | 通信 |
|---|---|---|---|
| KSU 模块卡片「打开」 | 平板上 | `module/webroot/index.html` | `ksu.exec()`，不占端口 |
| `http://<ip>:8089/` | 电脑上 | `/data/adb/edgefilter/web/index.html` | busybox httpd + CGI |

HTTP 那份是 `service.sh` 开机时从模块目录 `cp` 过去的副本，改了模块里的 `index.html`
要等下次开机（或手动跑一次 `service.sh`）才会同步。

浏览器版可以设访问令牌：往 `/data/adb/edgefilter/web.token` 写一行任意字符串即可。

「运行中」表示**虚拟设备已创建且 grab 成功**，而不只是「有个进程在」。只看 PID 会把
「抓住了面板但没有输出」误报成健康状态。

## 转屏

daemon 自己 fork 一个子进程每 2 秒读一次 `dumpsys input` 的 `Viewport INTERNAL orientation`，
值真的变了才通过管道通知主循环。`dumpsys` 一次约 45ms，放在独立进程里，
不占用触控事件循环。

主进程退出时子进程通过 `PR_SET_PDEATHSIG` 一起退出。

```
[edgefilter] rotation changed -> 90
```

## 排查

**边缘手势（侧滑返回 / 上滑回桌面）不灵**

按顺序查：

1. `MAJOR` 是否在 40~80 之间。低于指尖面积（12~36）会连手指一起拦。
2. 边缘带是否太宽。`BAND_LEFT=150` 约 1cm，调窄一些。
3. 临时全关验证：把四条边都设成 0，重启 daemon。

**触摸完全没反应**

```bash
su -c /data/adb/modules/edgefilter/ctl.sh status
su -c /data/adb/modules/edgefilter/ctl.sh restart
```

`ctl.sh stop` 立刻恢复原生触控，然后看 `/data/adb/edgefilter/edgefilter.log`。

日志里出现 `exiting; native touch restored` 说明 daemon 主动退出了，原因在同一行前面：

| 日志 | 含义 |
|---|---|
| `source SYN_DROPPED` | 我们读 event5 太慢，内核丢了事件；已退回原生 |
| `uinput write failed` | 虚拟设备写不进去；已退回原生 |
| `native contact is active` | 启动时手指正按着，拒绝启动；抬起后再 start |
| `invalid DOWN on slot` | 源设备事件流异常；已退回原生 |

**WebUI 打开是空白 / 不刷新**

刷新一次页面。还不行就看 `/data/adb/edgefilter/web/index.html` 是不是旧版本
（开机副本，见上文「WebUI」）。

## 已知限制

- 需要 root。抓住设备后，`getevent` 之类的原始读取读不到数据（被独占）。
- 触控笔（`event6/7`）不受影响，仍走原设备 —— 也就意味着**笔的误触不会拦**。
- 只在起手那一帧判定。手掌先落在中间再滑到边缘不会被拦。
- 贴边**原地点击**无法单独取消（会连滑动一起放行或一起拦掉）。这是为了不破坏触控流的
  主动取舍，见「设计原则」。

## 从源码构建

需要 Android NDK（arm64）、Python 3 和宿主 C 编译器（跑协议测试）：

```bash
./build.sh            # 跑测试 + 编译 + 打包 → dist/edgefilter-v<version>.zip
./build.sh --check    # 只做一致性校验 + 协议测试
./build.sh --no-cc    # 跳过交叉编译，直接用 module/bin/edgefilter
```

NDK 路径依次从 `ANDROID_NDK_HOME`、`$ANDROID_HOME/ndk/*`、`~/Android/Sdk/ndk/*`
自动查找。编译用 `aarch64-linux-android26-clang -static -O2`。

协议测试可以单独跑：

```bash
cc -std=gnu11 -w -o /tmp/t tests/protocol_test.c && /tmp/t
```

它把 daemon 的判定逻辑编译进宿主机，直接喂构造好的 protocol-B 事件帧，
断言输出流满足上文的不变量（含 40 组随机序列、共约 10 万条断言）。

推 `v*` tag 会自动构建并发布 Release（见 `.github/workflows/release.yml`）。

## 项目结构

```
.
├── .github/workflows/       # build（每次 push）/ release（v* tag）
├── build.sh                 # 校验 + 协议测试 + 编译 + 打包
├── docs/                    # 截图
├── src/
│   └── edgefilter.c         # daemon 源码（单文件，静态编译）
├── tests/
│   └── protocol_test.c      # protocol-B 协议回归测试（宿主运行）
└── module/                  # 模块载荷，zip 根目录即此目录
    ├── module.prop
    ├── service.sh           # 唯一完整启动入口
    ├── ctl.sh               # start/stop/restart/status/log（带文件锁）
    ├── web.sh               # busybox httpd 启停
    ├── api.sh               # 业务逻辑，输出 JSON（WebUI / CGI 共用）
    ├── action.sh            # 模块卡片「操作」按钮 → service.sh
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

It grabs the real touchscreen with `EVIOCGRAB` and re-emits everything through a uinput
multi-touch device that mirrors the source device's full capability set. Each contact is
judged **once**, on its first `SYN_REPORT`: a complete edge grip (position inside the band
and area ≥ `MAJOR`) is dropped for its whole lifetime; everything else is forwarded
untouched — same events, same order, same timestamps.

The design is deliberately small because the previous "cancel the tap, keep the swipe"
approach required delayed decisions, synthetic `DOWN` events and `SYN_DROPPED` cancellation.
Those cannot be made transparent: they cause click drift, ghost contacts and multi-touch
desync. That feature was removed on purpose.

Guarantees enforced by `tests/protocol_test.c`: unfiltered frames are byte-identical,
`BTN_TOUCH` follows only the contacts actually forwarded, a dropped contact leaks nothing,
incomplete first frames fail open, and any I/O error exits and restores native touch
instead of filtering with a corrupted state.

A bundled WebUI (KernelSU module card, or `http://<ip>:8089/`) shows live decisions on a
screen preview and lets you tune every value.

Requires root. Tested on Xiaomi Pad 7 Ultra (`jinghu`) / HyperOS 4 / Android 17 / KernelSU.
