# EdgeFilter 边缘防误触

[![build](https://github.com/zilewang7/hyperos4-edgefilter/actions/workflows/build.yml/badge.svg)](https://github.com/zilewang7/hyperos4-edgefilter/releases/latest)

KernelSU 模块。在用户态抓取触摸屏，把**贴边握持**和**贴边误触点击**拦掉，其余触点原样透传 ——
给**触控固件不支持原生 edge suppression** 的机型补上边缘防误触。

手里平板（Xiaomi Pad 7 Ultra / `jinghu`）握持时，虎口和拇指贴着边框，很容易误触。
小米框架层其实有 `EdgeSuppressionManager`，但这台机器的 `ro.vendor.touchfeature.type`
没有 edge mode 位，就算强行改了属性让框架实例化出来、也真的算出了抑制矩形，
**goodix 的固件和 HAL 根本不执行** —— 所以只能自己做。

| 能做 | 不能做 |
|---|---|
| 拦掉贴着边框的拇指 / 手掌按压 | 拦掉笔（`event6/7` 独立设备，不碰） |
| 拦掉贴边误触点击，同时保留边缘侧滑返回 | 需要改内核或刷分区（纯用户态，root 即可） |
| 转屏后自动重新映射 | 免解锁越狱的 KSU 重启后要手动拉起一次 |
| WebUI 可视化调参 + 实时判定预览 | |

## 设计原则

触控代理最容易犯的错，是**改写事件流**。v0.8 为了做到「点击取消、滑动放行」，
选择「先立刻转发 `DOWN`，抬手时再发 `SYN_DROPPED` 撤销」。这条路是错的：

- `SYN_DROPPED` 是**整台设备**的事件丢失通知，不是单指取消
- 撤销后本地状态清空但内核虚拟触点仍按住 → **幽灵触点**
- 多指时会把其他正在按的手指一起清掉 → **手势中断、跨 slot 跳变**

v0.10 改成**反方向**：核心带里的触点**先扣住不发**，动了才补发，没动就整段丢弃。

这个方向之所以安全，是因为**丢弃一个从未发出的触点不需要任何取消事件** ——
什么都不发就是干净的丢弃。所以：

- 没有 `SYN_DROPPED`
- 没有合成取消
- 没有幽灵触点
- 补发的 `DOWN` 单独占一帧，用的是**真实起手坐标**，所以不漂移

代价只有一个：核心带内的**滑动起手会晚 1~2 帧（约 8~16ms）**，因为要先确认手指确实在动。
实测侧滑返回、上滑回桌面均正常。

> 补发的 `DOWN` 必须**单独占一帧**。`InputReader` 每个 `SYN_REPORT` 只产生一个
> `MotionEvent`，如果把「起手点 + 当前点」塞进同一帧，系统只会看到当前点 ——
> `ACTION_DOWN` 就落在已经离开边缘的位置，返回手势不会触发。
> 这一点由 `tests/protocol_test.c` 断言保护。

## 原理

```
goodix_ts (event5)
      │  EVIOCGRAB 独占
      ▼
  edgefilter  ── 首帧判定 ──►  uinput: goodix_ts-filtered ──► InputReader ──► 应用
```

1. `EVIOCGRAB` 独占真实触摸屏，Android 收不到原始事件。
2. 建一个 uinput 虚拟多点触控设备（protocol B），**能力位完全复制源设备**。
3. 每个触点在**起手那一帧**判定一次，之后整段转发或整段丢弃。

### 硬性不变量

由 `tests/protocol_test.c` 断言（约 12 万条）：

| 不变量 | 意义 |
|---|---|
| 未被过滤的帧逐事件、逐字节不变 | 时间戳、顺序、坐标都不重排 |
| 永不发出 `SYN_DROPPED` | 不做设备级取消 |
| `BTN_TOUCH` / `BTN_TOOL_FINGER` 只由**实际输出的触点**决定 | 不盲传源设备全局按键 |
| 被丢弃的触点：零 `DOWN`、零坐标、零 legacy 轴 | 不泄漏任何痕迹 |
| 一个放行触点恰好一个 `DOWN`、一个 `UP` | 不重复、不丢失 |
| 补发的 `DOWN` 独占一帧且位于真实起手坐标 | `ACTION_DOWN` 落在边缘，返回手势能识别 |
| 首帧缺坐标或面积 → **放行** | 绝不用旧值猜、绝不延迟 |
| 写入失败 / 源 `SYN_DROPPED` / 帧溢出 → **退出并恢复原生** | 绝不带着失真状态继续独占面板 |
| `grabbed` ⇒ 虚拟设备已就绪 | 不会出现「抓住面板但没输出」 |

启动时还会检查原生触点状态：**如果手指正按着，拒绝启动并保持原生触控**，等抬起后再启动。
`EVIOCGRAB` 不会替其他客户端取消已经收到的 `DOWN`，从半段手势开始转发会造成残留。

### 为什么不再处理亮灭屏

虚拟设备在亮灭屏之间**保持不变**，也不释放 grab。源设备的全部按键能力（包括双击唤醒的
`KEY_WAKEUP`、`KEY_POWER`）都复制并原样透传。

v0.8 在息屏时释放 grab、销毁 uinput，亮屏时重建。反复拆装输入设备会扰动
InputReader 与触摸 IC 的模式状态，是「亮屏后触摸无响应」的可疑来源之一。

## 两层边缘带

```
        ┌───────────────────────────────┐
        │██│                         │██│   核心带 CORE_* → 扣住：动了放行，没动丢弃
        │▓▓│                         │▓▓│   外层带 BAND_* → 面积 ≥ MAJOR 才拦
        │  │                         │  │
        │  │        可触摸区          │  │
        │  │                         │  │
        │▓▓│                         │▓▓│
        │██│                         │██│
        └───────────────────────────────┘
```

判定顺序：**先看握持，再看核心带，最后放行**。

1. **外层带内且面积 ≥ `MAJOR`** → 判定为握持，整段丢弃，零延迟。
2. **核心带内**（且不满足 1）→ 先扣住：
   - 位移超过 `CORE_SLOP` → 判定为滑动，补发起手点后完全放行
   - 扣住超过 `CORE_HOLD_MS` 仍未移动 → 放行（保证长按可用）
   - 没动就抬手 → 判定为误触点击，整段丢弃
3. 其余 → 直接放行。

### `MAJOR` 怎么定

这台机器实测：

| | 面积 `ABS_MT_TOUCH_MAJOR` |
|---|---|
| 指尖点击 / 侧滑起手 | 12 ~ 36 |
| 拇指贴着边框握持 | 40 ~ 76 |

所以 `MAJOR=40` 正好：握持边框的拇指被拦，指尖操作照常。
调到 76 以上握持时会漏；设成 `0` 则边缘带内全拦，边缘手势全废。

### `CORE_*` 怎么定

核心带只该盖住「贴着边框那一小条」，`40` 约 6mm。必须 ≤ 对应的 `BAND_*`（WebUI 会自动 clamp）。

`CORE_HOLD_MS=0` 可完全关闭核心带，退回纯边缘带过滤（v0.9 行为）。

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
adb push edgefilter-v0.10.1.zip /data/local/tmp/
adb shell 'su -c "ksud module install /data/local/tmp/edgefilter-v0.10.1.zip"'
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
| `BAND_LEFT` / `BAND_RIGHT` | 150 | 外层左右边缘带宽度（逻辑像素，150 ≈ 1cm） |
| `BAND_TOP` / `BAND_BOTTOM` | 0 | 外层上下边缘带，0 = 不拦 |
| `MAJOR` | 40 | 外层带内的面积阈值；0 = 外层带内全拦 |
| `CORE_LEFT` / `CORE_RIGHT` | 40 | 核心带宽度；0 = 该边没有核心带 |
| `CORE_TOP` / `CORE_BOTTOM` | 0 | 核心带上下 |
| `CORE_SLOP` | 12 | 核心带内移动超过这么多 px 就判定为滑动并放行 |
| `CORE_HOLD_MS` | 200 | 扣住上限；超时放行（保证长按可用）。0 = 关闭核心带 |
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
不占用触控事件循环。主进程退出时子进程通过 `PR_SET_PDEATHSIG` 一起退出。

## 排查

**边缘手势（侧滑返回 / 上滑回桌面）不灵**

1. `CORE_HOLD_MS` 设成 `0`，确认是否为核心带引起。
2. `CORE_SLOP` 调小（比如 6），让滑动更快被识别。
3. 核心带是否比外层带还宽 —— WebUI 会自动 clamp，手改配置要注意。
4. `MAJOR` 是否在 40~80 之间；低于指尖面积（12~36）会连手指一起拦。

**贴边点击没有被拦**

1. 核心带宽度是否为 0。
2. 落点是否真的在核心带内（WebUI 的实时判定会画出红点）。
3. `CORE_HOLD_MS` 是否太小 —— 按得太久会被当作长按放行。

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
- 核心带内的滑动起手会晚 1~2 帧（约 8~16ms）。
- 核心带内的长按会在 `CORE_HOLD_MS` 后放行，不是从按下那一刻算起。
- 如果面板在手指静止时完全不发帧，核心带内的长按会一直扣到抬手（表现为长按无效）。

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
断言输出流满足上文的不变量（含 40 组随机序列）。

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
multi-touch device that mirrors the source device's full capability set.

Two nested bands, judged once per contact at its first `SYN_REPORT`:

- **Outer band** — a contact whose area is ≥ `MAJOR` is a grip and is dropped immediately.
- **Core band** — the contact is **held**, not forwarded. It is released (with its `DOWN`
  replayed at the true first position) once it moves more than `CORE_SLOP`, so edge back
  gestures still work. If it lifts without moving it is an accidental tap and is dropped.
- Everything else is forwarded byte-for-byte.

Holding is the safe direction: a contact that was never forwarded can be discarded by simply
emitting nothing, so there is no `SYN_DROPPED`, no synthetic cancel and no ghost contact.
The only cost is that a swipe starting inside the core band begins 1–2 frames late (~8–16 ms).

Guarantees enforced by `tests/protocol_test.c`: unfiltered frames are byte-identical,
`SYN_DROPPED` is never emitted, `BTN_TOUCH` follows only the contacts actually forwarded,
a dropped contact leaks nothing, replayed `DOWN` events use the true origin, incomplete
first frames fail open, and any I/O error exits and restores native touch.

A bundled WebUI (KernelSU module card, or `http://<ip>:8089/`) shows live decisions on a
screen preview and lets you tune every value.

Requires root. Tested on Xiaomi Pad 7 Ultra (`jinghu`) / HyperOS 4 / Android 17 / KernelSU.
