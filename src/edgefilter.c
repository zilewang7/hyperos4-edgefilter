/*
 * edgefilter v2 - userspace edge touch filter for Android touchscreens
 *
 * Grabs the real touchscreen (EVIOCGRAB), drops contacts whose FIRST touch
 * point falls inside a configurable edge band, and re-emits everything else
 * through a uinput virtual multitouch device (protocol B).
 *
 * Root required. Build: aarch64-linux-android26-clang -static -O2
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <linux/uinput.h>

#define MAX_SLOTS   16
#define MAX_FRAME   1024

static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_on = 0;      /* SIGHUP  : screen turned on  */
static volatile sig_atomic_t g_off = 0;     /* SIGUSR1 : screen turned off */
static volatile sig_atomic_t g_rot = 0;     /* SIGUSR2 : 配置变了，重读旋转 */
static void on_sig(int s) { (void)s; g_stop = 1; }
static void on_hup(int s) { (void)s; g_on = 1; }
static void on_usr1(int s) { (void)s; g_off = 1; }
static void on_usr2(int s) { (void)s; g_rot = 1; }

static int grabbed = 0;                     /* EVIOCGRAB currently held */

static char g_name[UINPUT_MAX_NAME_SIZE] = {0};
static int g_bus, g_vendor, g_product, g_version;

static int src_fd = -1;
static int ui_fd  = -1;

struct slot {
    int active, blocked, new_in_frame, x, y, have_x, have_y, major;
    int undecided;   /* 坐标还没到齐，先扣住不判 */
    int wait;        /* 已经等了几个 SYN 帧 */
    int held;        /* 是否被扣住过（需要补发 DOWN） */
    /* 核心带延后判定：先挂起，移动超过 slop 当手势放行，原地抬手当点击拦截 */
    int pending, sx, sy, smajor, tid, synth;
};
static struct slot slots[MAX_SLOTS];
static int cur_slot = 0;

struct fev { struct input_event ev; int slot; int btn; int pass; };
static struct fev frame[MAX_FRAME];
static int frame_n = 0;
static int emitted_down = 0;
static int ui_dead = 0;          /* uinput device gone -> recreate */
static int n_keys = 0, n_abs = 0;
static unsigned long g_dropped = 0;
static int g_cancel = 0;        /* 下一帧发 SYN_DROPPED，把这次触摸取消掉 */

/* config */
static int band_l = 0, band_r = 0, band_t = 0, band_b = 0;   /* logical px */
static int core_l = 0, core_r = 0, core_t = 0, core_b = 0;   /* logical px */
static int core_slop = 12;                                   /* logical px, 0 = 核心带全拦 */
static double scale = 10.0;                                  /* raw units per px */
static int rot = 1;                                          /* 0..3 -> 0/90/180/270 */
static int major_min = 0;
static int max_x = 0, max_y = 0;
static int verbose = 0;
static int dry_run = 0;

static void logmsg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[edgefilter] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    fflush(stderr);
}

/* 整帧批量写：一个 write() 把整帧交给 uinput。
 * 原来每个事件一次 write，一帧动辄 20+ 次系统调用，忙的时候读 event5 跟不上，
 * 内核就会往我们这边注入 SYN_DROPPED。 */
static struct input_event obuf[MAX_FRAME * 2];
static int obuf_n = 0;

static void flush_out(void)
{
    if (obuf_n == 0) return;
    int n = obuf_n;
    obuf_n = 0;
    if (dry_run || ui_fd < 0) return;
    ssize_t w = write(ui_fd, obuf, (size_t)n * sizeof(struct input_event));
    if (w < 0 && errno != EAGAIN) {
        logmsg("uinput write failed: %s", strerror(errno));
        if (errno == ENODEV) ui_dead = 1;
    }
}

static void ui_write(const struct input_event *ev)
{
    if (dry_run || ui_fd < 0) return;
    if (obuf_n >= (int)(sizeof(obuf) / sizeof(obuf[0]))) flush_out();
    if (obuf_n >= (int)(sizeof(obuf) / sizeof(obuf[0]))) return;
    obuf[obuf_n++] = *ev;
}

static void emit(int type, int code, int value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type; ev.code = code; ev.value = value;
    ui_write(&ev);
}

/* rotation-aware edge test; l/r/t/b are logical px on the four screen edges */
static int in_edges(int x, int y, int l, int r, int t, int b)
{
    int L = (int)(l * scale), R = (int)(r * scale);
    int T = (int)(t * scale), B = (int)(b * scale);
    switch (rot) {
    case 0:  /* portrait */
        return (L > 0 && x < L) || (R > 0 && x > max_x - R) ||
               (T > 0 && y < T) || (B > 0 && y > max_y - B);
    case 1:  /* 90: screen-left = panel y-min, screen-top = panel x-max */
        return (L > 0 && y < L) || (R > 0 && y > max_y - R) ||
               (T > 0 && x > max_x - T) || (B > 0 && x < B);
    case 2:  /* 180 */
        return (L > 0 && x > max_x - L) || (R > 0 && x < R) ||
               (T > 0 && y > max_y - T) || (B > 0 && y < B);
    default: /* 270 */
        return (L > 0 && y > max_y - L) || (R > 0 && y < R) ||
               (T > 0 && x < T) || (B > 0 && x > max_x - B);
    }
}

/* outer band: area-based filtering (edge grip) */
static int in_zone(int x, int y)
{
    return in_edges(x, y, band_l, band_r, band_t, band_b);
}

/* inner "core" band: block anything that starts here, area ignored */
static int in_core(int x, int y)
{
    return in_edges(x, y, core_l, core_r, core_t, core_b);
}

static int active_unblocked(void)
{
    int n = 0;
    for (int i = 0; i < MAX_SLOTS; i++)
        if (slots[i].active && !slots[i].blocked) n++;
    return n;
}

static void reset_silent(void)
{
    for (int i = 0; i < MAX_SLOTS; i++) {
        slots[i].active = 0; slots[i].blocked = 0;
        slots[i].have_x = 0; slots[i].have_y = 0;
        slots[i].new_in_frame = 0; slots[i].major = 0;
        slots[i].pending = 0; slots[i].synth = 0;
        slots[i].undecided = 0; slots[i].wait = 0; slots[i].held = 0;
    }
    frame_n = 0;
    cur_slot = 0;
    emitted_down = 0;
    obuf_n = 0;
}

static void reset_state(void)
{
    /* lift every contact the virtual device still holds, so InputReader can
     * never be left with a phantom finger down */
    int any = 0;
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (slots[i].active) {
            emit(EV_ABS, ABS_MT_SLOT, i);
            emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
            any = 1;
        }
        slots[i].active = 0;
        slots[i].blocked = 0;
        slots[i].have_x = 0;
        slots[i].have_y = 0;
        slots[i].undecided = 0;
        slots[i].new_in_frame = 0;
        slots[i].major = 0;
        slots[i].pending = 0;
        slots[i].synth = 0;
    }
    if (emitted_down) {
        emit(EV_KEY, BTN_TOUCH, 0);
        emit(EV_KEY, BTN_TOOL_FINGER, 0);
        emitted_down = 0;
        any = 1;
    }
    if (any) emit(EV_SYN, SYN_REPORT, 0);
    flush_out();
    frame_n = 0;
    cur_slot = 0;
}

static void flush_frame(void)
{
    if (g_cancel) {
        /* 核心带里原地点击：用 SYN_DROPPED 让 Android 取消这次触摸。
         * 应用收到 ACTION_CANCEL，不会触发点击；同时因为 DOWN 是立刻转发出去的，
         * 侧滑/上滑这些手势完全不受影响。 */
        g_cancel = 0;
        frame_n = 0;
        emit(EV_SYN, SYN_DROPPED, 0);
        flush_out();
        reset_silent();
        return;
    }

    if (frame_n == 0) { emit(EV_SYN, SYN_REPORT, 0); flush_out(); return; }

    int wrote = 0;

    /* 1. pass-through events first: KEY_WAKEUP (double-tap gesture), ABS_X/Y,
     *    SYN_DROPPED, ...  These must never be filtered. */
    for (int i = 0; i < frame_n; i++) {
        if (!frame[i].pass) continue;
        ui_write(&frame[i].ev);
        wrote = 1;
    }

    for (int i = 0; i < MAX_SLOTS; i++) {
        if (!slots[i].new_in_frame) continue;

        /* 面板常常先报 POSITION_X，POSITION_Y 晚 1~4 帧才来。
         * 坐标没到齐就做判定，会拿上一个触点的残留值当坐标 —— 时对时错。
         * 所以这里先扣住，等两个轴都到齐再判。 */
        if (slots[i].undecided) {
            if (!slots[i].have_x || !slots[i].have_y) {
                if (slots[i].wait < 3) {
                    slots[i].wait++;
                    slots[i].held = 1;     /* 记住扣过，放行时要补 DOWN */
                    slots[i].blocked = 1;  /* 扣住，不转发 */
                    continue;              /* new_in_frame 保持，下帧再判 */
                }
                /* 等了 3 帧还不齐，就用现有值判，不能无限期扣着 */
            }
            slots[i].undecided = 0;
        }

        int blocked = 0;
        const char *why = "pass";
        if (in_core(slots[i].x, slots[i].y)) {
            if (core_slop > 0) {
                /* 核心带：立刻放行，但记下来。如果它一直没怎么动就抬手，
                 * 说明是原地误触 —— 到抬手时用 SYN_DROPPED 取消掉。
                 * 这样手势拿到的是零延迟的真实 DOWN，点击又不会生效。 */
                slots[i].pending = 1;
                slots[i].sx = slots[i].x;
                slots[i].sy = slots[i].y;
                slots[i].smajor = slots[i].major;
                blocked = 0; why = "core";
            } else {
                blocked = 1; why = "BLOCKED core";
            }
        } else if (in_zone(slots[i].x, slots[i].y)) {
            if (major_min > 0) {
                blocked = (slots[i].major >= major_min);  /* 贴边握持才拦 */
                why = blocked ? "BLOCKED grip" : "pass";
            } else {
                blocked = 1;                              /* block everything in band */
                why = "BLOCKED band";
            }
        }
        slots[i].blocked = blocked;
        if (!blocked && slots[i].held) {
            /* 前面被扣住的 DOWN 要补发，否则 Android 永远看不到这次触摸。
             * 没被扣过的不要补，否则会多发一次 TRACKING_ID。 */
            slots[i].sx = slots[i].x;
            slots[i].sy = slots[i].y;
            slots[i].synth = 1;
            slots[i].held = 0;
        }
        slots[i].new_in_frame = 0;
        if (strcmp(why, "core?") != 0)
            logmsg("slot %d new contact at (%d,%d) major=%d -> %s",
                   i, slots[i].x, slots[i].y, slots[i].major, why);
    }

    int active = active_unblocked();

    if (!emitted_down && active > 0) {
        emit(EV_KEY, BTN_TOUCH, 1);
        emit(EV_KEY, BTN_TOOL_FINGER, 1);
        emitted_down = 1; wrote = 1;
    }

    /* 核心带里被放行的触点：补一个 DOWN，位置用它最初的落点，
     * 这样手势识别器看到的起点和真实起手点一致。 */
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (!slots[i].synth) continue;
        emit(EV_ABS, ABS_MT_SLOT, i);
        emit(EV_ABS, ABS_MT_TRACKING_ID, slots[i].tid);
        emit(EV_ABS, ABS_MT_POSITION_X, slots[i].sx);
        emit(EV_ABS, ABS_MT_POSITION_Y, slots[i].sy);
        slots[i].synth = 0;
        wrote = 1;
    }
    for (int i = 0; i < frame_n; i++) {
        struct fev *f = &frame[i];
        if (f->pass || f->btn) continue;
        if (f->slot >= 0 && slots[f->slot].blocked) continue;
        ui_write(&f->ev);
        wrote = 1;
    }
    if (emitted_down && active == 0) {
        emit(EV_KEY, BTN_TOUCH, 0);
        emit(EV_KEY, BTN_TOOL_FINGER, 0);
        emitted_down = 0; wrote = 1;
    }
    if (wrote) emit(EV_SYN, SYN_REPORT, 0);
    flush_out();

    for (int i = 0; i < MAX_SLOTS; i++) {
        slots[i].new_in_frame = 0;
        if (!slots[i].active) slots[i].blocked = 0;
    }
    frame_n = 0;
}

static void add_pass(const struct input_event *ev)
{
    if (frame_n >= MAX_FRAME) return;
    frame[frame_n].ev = *ev;
    frame[frame_n].slot = -1;
    frame[frame_n].btn = 0;
    frame[frame_n].pass = 1;
    frame_n++;
}

static void handle_event(const struct input_event *ev)
{
    if (ev->type == EV_SYN) {
        if (ev->code == SYN_REPORT) { flush_frame(); return; }
        if (ev->code == SYN_DROPPED) {
            /* 内核 evdev 缓冲溢出（我们读慢了）。绝对不能原样转发给 Android：
             * InputReader 收到 SYN_DROPPED 会把正在进行的触摸 CANCEL 掉，
             * 侧滑返回就是这样被掐断的。这里只重同步自己的状态。 */
            g_dropped++;
            logmsg("SYN_DROPPED from source (#%d) -> resync", g_dropped);
            reset_state();
            return;
        }
        add_pass(ev);
        return;
    }

    int slot = -1, btn = 0;
    if (ev->type == EV_ABS) {
        switch (ev->code) {
        case ABS_MT_SLOT:
            if (ev->value >= 0 && ev->value < MAX_SLOTS) cur_slot = ev->value;
            break;
        case ABS_MT_TRACKING_ID:
            if (ev->value >= 0) {
                slots[cur_slot].active = 1;
                slots[cur_slot].new_in_frame = 1;
                slots[cur_slot].have_x = 0;
                slots[cur_slot].have_y = 0;
                slots[cur_slot].undecided = 1;   /* 等两个坐标到齐 */
                slots[cur_slot].wait = 0;
                slots[cur_slot].held = 0;
                slots[cur_slot].x = 0;
                slots[cur_slot].y = 0;
                slots[cur_slot].blocked = 1;     /* 未定之前一律扣住 */
                slots[cur_slot].major = 0;
                slots[cur_slot].pending = 0;
                slots[cur_slot].synth = 0;
                slots[cur_slot].tid = ev->value;
            } else {
                /* 抬手时还处于 pending = 它一直没怎么动 -> 当点击，拦掉 */
                if (slots[cur_slot].pending) {
                    logmsg("slot %d core tap at (%d,%d) major=%d -> BLOCKED core",
                           cur_slot, slots[cur_slot].sx, slots[cur_slot].sy,
                           slots[cur_slot].smajor);
                    g_cancel = 1;      /* 用 CANCEL 抹掉这次点击 */
                    slots[cur_slot].active = 0;
                    slots[cur_slot].pending = 0;
                    return;            /* 不要把 TRACKING_ID -1 发出去 */
                }
                slots[cur_slot].active = 0;
                slots[cur_slot].have_x = 0;
                slots[cur_slot].have_y = 0;
                slots[cur_slot].undecided = 0;
                slots[cur_slot].pending = 0;
                slots[cur_slot].synth = 0;
            }
            break;
        case ABS_MT_POSITION_X: slots[cur_slot].x = ev->value; slots[cur_slot].have_x = 1; break;
        case ABS_MT_POSITION_Y: slots[cur_slot].y = ev->value; slots[cur_slot].have_y = 1; break;
        case ABS_MT_TOUCH_MAJOR: slots[cur_slot].major = ev->value; break;
        default:
            /* ABS_X / ABS_Y / anything else the panel reports outside MT:
             * forward untouched. */
            add_pass(ev);
            return;
        }
        slot = cur_slot;
    } else if (ev->type == EV_KEY) {
        if (ev->code == BTN_TOUCH || ev->code == BTN_TOOL_FINGER) btn = 1;
        else {
            /* KEY_WAKEUP (double-tap-to-wake), KEY_POWER, ... : never filter */
            add_pass(ev);
            return;
        }
    } else {
        add_pass(ev);
        return;
    }

    /* 核心带 pending 判定：动了就说明是手势，放行 */
    if (slots[cur_slot].pending && slots[cur_slot].have_x && slots[cur_slot].have_y) {
        int slop = (int)(core_slop * scale);
        int dx = slots[cur_slot].x - slots[cur_slot].sx;
        int dy = slots[cur_slot].y - slots[cur_slot].sy;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx > slop || dy > slop) {
            slots[cur_slot].pending = 0;
            slots[cur_slot].blocked = 0;
            logmsg("slot %d core swipe at (%d,%d) major=%d -> pass",
                   cur_slot, slots[cur_slot].sx, slots[cur_slot].sy,
                   slots[cur_slot].smajor);
        }
    }

    if (frame_n < MAX_FRAME) {
        frame[frame_n].ev = *ev;
        frame[frame_n].slot = slot;
        frame[frame_n].btn = btn;
        frame[frame_n].pass = 0;
        frame_n++;
    }
}

static int read_abs(int fd, int code, struct input_absinfo *ai)
{
    memset(ai, 0, sizeof(*ai));
    return ioctl(fd, EVIOCGABS(code), ai);
}

static int has_abs(int fd, int code)
{
    unsigned long bits[ABS_CNT / (8 * sizeof(long)) + 1];
    memset(bits, 0, sizeof(bits));
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(bits)), bits) < 0) return 0;
    return (bits[code / (8 * sizeof(long))] >> (code % (8 * sizeof(long)))) & 1UL;
}

/* keep has_abs referenced even though copy_caps() uses the raw bitmap */
__attribute__((unused)) static int (*const has_abs_ref)(int, int) = has_abs;

/* copy the source device's whole capability set onto the virtual device,
 * so that non-touch events (KEY_WAKEUP double-tap gesture, ABS_X/ABS_Y ...)
 * survive the proxy hop. */
static void copy_caps(void)
{
    unsigned long kbits[KEY_CNT / (8 * sizeof(long)) + 1];
    unsigned long abits[ABS_CNT / (8 * sizeof(long)) + 1];
    int c;

    memset(kbits, 0, sizeof(kbits));
    memset(abits, 0, sizeof(abits));
    if (ioctl(src_fd, EVIOCGBIT(EV_KEY, sizeof(kbits)), kbits) < 0)
        logmsg("EVIOCGBIT(EV_KEY) failed: %s", strerror(errno));
    if (ioctl(src_fd, EVIOCGBIT(EV_ABS, sizeof(abits)), abits) < 0)
        logmsg("EVIOCGBIT(EV_ABS) failed: %s", strerror(errno));

    for (c = 0; c < KEY_CNT; c++) {
        if (!(kbits[c / (8 * sizeof(long))] & (1UL << (c % (8 * sizeof(long)))))) continue;
        ioctl(ui_fd, UI_SET_KEYBIT, c);
        n_keys++;
    }
    for (c = 0; c < ABS_CNT; c++) {
        struct input_absinfo ai;
        if (!(abits[c / (8 * sizeof(long))] & (1UL << (c % (8 * sizeof(long)))))) continue;
        ioctl(ui_fd, UI_SET_ABSBIT, c);
        n_abs++;
        if (read_abs(src_fd, c, &ai) < 0) memset(&ai, 0, sizeof(ai));
#ifdef UI_ABS_SETUP
        struct uinput_abs_setup uas;
        memset(&uas, 0, sizeof(uas));
        uas.code = c;
        uas.absinfo = ai;
        if (ioctl(ui_fd, UI_ABS_SETUP, &uas) < 0)
            logmsg("UI_ABS_SETUP(%d) failed: %s", c, strerror(errno));
#endif
    }
    /* BTN_TOUCH / BTN_TOOL_FINGER must always exist even if the source omits them */
    ioctl(ui_fd, UI_SET_KEYBIT, BTN_TOUCH);
    ioctl(ui_fd, UI_SET_KEYBIT, BTN_TOOL_FINGER);
}

static int setup_uinput(const char *src_name, int bus, int vendor, int product, int version)
{
    struct uinput_setup usetup;

    ui_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (ui_fd < 0) { logmsg("open /dev/uinput failed: %s", strerror(errno)); return -1; }

    ioctl(ui_fd, UI_SET_EVBIT, EV_KEY);
    ioctl(ui_fd, UI_SET_EVBIT, EV_ABS);
    ioctl(ui_fd, UI_SET_EVBIT, EV_SYN);
    n_keys = n_abs = 0;
    copy_caps();
    ioctl(ui_fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);

    memset(&usetup, 0, sizeof(usetup));
    snprintf(usetup.name, UINPUT_MAX_NAME_SIZE, "%s-filtered", src_name);
    usetup.id.bustype = bus;
    usetup.id.vendor = vendor;
    usetup.id.product = product;
    usetup.id.version = version;
    if (ioctl(ui_fd, UI_DEV_SETUP, &usetup) < 0) { logmsg("UI_DEV_SETUP failed: %s", strerror(errno)); return -1; }
    if (ioctl(ui_fd, UI_DEV_CREATE) < 0) { logmsg("UI_DEV_CREATE failed: %s", strerror(errno)); return -1; }
    return 0;
}

static int find_device_by_name(const char *want, char *out, size_t outlen)
{
    DIR *d = opendir("/dev/input");
    if (!d) return -1;
    struct dirent *de;
    char path[256], name[128];
    int found = -1;
    while ((de = readdir(d))) {
        if (strncmp(de->d_name, "event", 5)) continue;
        snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        memset(name, 0, sizeof(name));
        if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 && !strcmp(name, want)) {
            snprintf(out, outlen, "%s", path);
            found = 0;
            close(fd);
            break;
        }
        close(fd);
    }
    closedir(d);
    return found;
}

static void write_info(void)
{
    FILE *f = fopen("/data/adb/edgefilter/info", "w");
    if (!f) return;
    fprintf(f, "%d %d %d %s\n", max_x, max_y, rot * 90, g_name);
    fclose(f);
}

static int detect_rotation(void)
{
    FILE *p = popen("dumpsys input 2>/dev/null | grep -m1 'Viewport INTERNAL'", "r");
    if (!p) return -1;
    char line[512];
    int r = -1;
    if (fgets(line, sizeof(line), p)) {
        char *q = strstr(line, "orientation=");
        if (q) {
            int v = atoi(q + 12);
            if (v >= 0 && v <= 3) r = v;
        }
    }
    pclose(p);
    return r;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: edgefilter [-d DEV | --name NAME] [--left N] [--right N] [--top N] [--bottom N]\n"
        "                  [--core-left N] [--core-right N] [--core-top N] [--core-bottom N]\n"
        "                  [--scale F] [--rot auto|0|90|180|270] [--major N]\n"
        "                  [-v] [--dry-run] [--info]\n"
        "  bands are in logical screen px; --scale converts to raw units (default 10)\n"
        "  --left/--right/--top/--bottom : outer band, only contacts with area >= --major\n"
        "  --core-* : inner band nested inside the outer one\n"
        "  --core-slop N : 核心带内移动超过 N px 视为手势放行（0 = 一律拦截）\n"
        "  --major  : ABS_MT_TOUCH_MAJOR threshold for the outer band (0 = block all)\n"
        "             实测：指尖 12~36，拇指贴边握持 40~76\n");
}

int main(int argc, char **argv)
{
    const char *dev = NULL, *name = "goodix_ts";
    char devpath[256] = {0};
    char src_name[UINPUT_MAX_NAME_SIZE] = {0};
    struct input_id id;
    struct input_absinfo aix, aiy;
    int info_only = 0, rot_opt = -1;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc) dev = argv[++i];
        else if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(argv[i], "--left") && i + 1 < argc) band_l = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--right") && i + 1 < argc) band_r = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--top") && i + 1 < argc) band_t = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bottom") && i + 1 < argc) band_b = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--core-left") && i + 1 < argc) core_l = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--core-right") && i + 1 < argc) core_r = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--core-top") && i + 1 < argc) core_t = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--core-bottom") && i + 1 < argc) core_b = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--core-slop") && i + 1 < argc) core_slop = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atof(argv[++i]);
        else if (!strcmp(argv[i], "--major") && i + 1 < argc) major_min = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rot") && i + 1 < argc) {
            const char *v = argv[++i];
            if (!strcmp(v, "auto")) rot_opt = -1;
            else rot_opt = atoi(v) / 90;
        }
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "--dry-run")) dry_run = 1;
        else if (!strcmp(argv[i], "--info")) info_only = 1;
        else { usage(); return 2; }
    }

    if (!dev) {
        if (find_device_by_name(name, devpath, sizeof(devpath)) < 0) {
            logmsg("no input device named '%s'", name);
            return 1;
        }
        dev = devpath;
    }

    src_fd = open(dev, O_RDONLY | O_CLOEXEC);
    if (src_fd < 0) { logmsg("open %s failed: %s", dev, strerror(errno)); return 1; }

    ioctl(src_fd, EVIOCGNAME(sizeof(src_name)), src_name);
    ioctl(src_fd, EVIOCGID, &id);
    g_bus = id.bustype; g_vendor = id.vendor; g_product = id.product; g_version = id.version;
    snprintf(g_name, sizeof(g_name), "%s", src_name);
    if (read_abs(src_fd, ABS_MT_POSITION_X, &aix) < 0 ||
        read_abs(src_fd, ABS_MT_POSITION_Y, &aiy) < 0) {
        logmsg("%s has no ABS_MT_POSITION_X/Y", dev);
        return 1;
    }
    max_x = aix.maximum;
    max_y = aiy.maximum;

    if (rot_opt >= 0) rot = rot_opt;
    else {
        int r = detect_rotation();
        if (r >= 0) rot = r;
    }

    logmsg("source %s (%s) panel 0..%d x 0..%d  rot=%d  scale=%.1f",
           dev, src_name, max_x, max_y, rot * 90, scale);
    logmsg("bands L=%d R=%d T=%d B=%d px  core L=%d R=%d T=%d B=%d px  slop=%d  major_min=%d dry_run=%d",
           band_l, band_r, band_t, band_b, core_l, core_r, core_t, core_b, core_slop, major_min, dry_run);

    /* 给 api.sh 用的小文件，省得每次去 grep 整份日志 */
    write_info();

    if (info_only) return 0;

    if (!dry_run) {
        if (setup_uinput(src_name, id.bustype, id.vendor, id.product, id.version) < 0) return 1;
        if (ioctl(src_fd, EVIOCGRAB, 1) < 0) {
            logmsg("EVIOCGRAB failed: %s", strerror(errno));
            return 1;
        }
        logmsg("grabbed %s, virtual device '%s-filtered' created (%d keys, %d abs)",
               dev, src_name, n_keys, n_abs);
        grabbed = 1;
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGHUP, on_hup);
    signal(SIGUSR1, on_usr1);
    signal(SIGUSR2, on_usr2);

    struct pollfd pfd = { .fd = src_fd, .events = POLLIN };
    struct input_event buf[256];

    while (!g_stop) {
        if (g_off) {
            /* Screen off: hand the panel back to Android untouched so that the
             * double-tap-to-wake gesture, doze mode and the whole native
             * gesture path work exactly as they would without us. */
            g_off = 0;
            if (grabbed) {
                ioctl(src_fd, EVIOCGRAB, 0);
                grabbed = 0;
                logmsg("screen off: grab released (native gesture path)");
            }
            if (ui_fd >= 0) {
                ioctl(ui_fd, UI_DEV_DESTROY);
                close(ui_fd);
                ui_fd = -1;
                logmsg("screen off: virtual device removed");
            }
            reset_state();
        }
        if (g_rot) {
            /* 旋转可能变了：重读一次，变了就更新 info 让 WebUI 也跟着变 */
            g_rot = 0;
            if (rot_opt < 0) {                 /* 只有 ROT=auto 才自动跟 */
                int nr = detect_rotation();
                if (nr >= 0 && nr != rot) {
                    logmsg("rotation changed %d -> %d", rot * 90, nr * 90);
                    rot = nr;
                    write_info();
                }
            }
        }
        if (g_on) {
            g_on = 0;
            reset_state();
            if (rot_opt < 0) {
                int nr = detect_rotation();
                if (nr >= 0 && nr != rot) {
                    logmsg("rotation changed %d -> %d", rot * 90, nr * 90);
                    rot = nr;
                    write_info();
                }
            }
            if (ui_fd < 0 && setup_uinput(g_name, g_bus, g_vendor, g_product, g_version) < 0)
                logmsg("screen on: creating virtual device failed");
            if (!grabbed) {
                if (ioctl(src_fd, EVIOCGRAB, 1) < 0)
                    logmsg("screen on: EVIOCGRAB failed: %s", strerror(errno));
                else {
                    grabbed = 1;
                    logmsg("screen on: grabbed (%d keys, %d abs)", n_keys, n_abs);
                }
            } else {
                logmsg("screen on: contact state reset");
            }
        }
        if (ui_dead) {
            ui_dead = 0;
            logmsg("uinput device lost, recreating");
            reset_state();
            if (ui_fd >= 0) { ioctl(ui_fd, UI_DEV_DESTROY); close(ui_fd); ui_fd = -1; }
            if (setup_uinput(g_name, g_bus, g_vendor, g_product, g_version) < 0) {
                /* never leave the panel grabbed without a working sink */
                logmsg("recreate failed -> releasing grab, native touch restored");
                ioctl(src_fd, EVIOCGRAB, 0);
                grabbed = 0;
                break;
            }
            logmsg("uinput recreated (%d keys, %d abs)", n_keys, n_abs);
        }
        int pr = poll(&pfd, 1, 500);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) continue;
        ssize_t r = read(src_fd, buf, sizeof(buf));
        if (r <= 0) {
            if (r < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            logmsg("read failed: %s", strerror(errno));
            break;
        }
        /* while ungrabbed Android is reading the same events natively:
         * forwarding them would duplicate every touch, so drop them. */
        if (!grabbed) continue;
        int n = (int)(r / (ssize_t)sizeof(struct input_event));
        for (int i = 0; i < n; i++) handle_event(&buf[i]);
    }

    logmsg("exiting");
    if (ui_fd >= 0) { ioctl(ui_fd, UI_DEV_DESTROY); close(ui_fd); }
    if (src_fd >= 0) { ioctl(src_fd, EVIOCGRAB, 0); close(src_fd); }
    return 0;
}
