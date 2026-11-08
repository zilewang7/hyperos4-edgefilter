/*
 * EdgeFilter: a deliberately small evdev -> uinput contact filter.
 *
 * A new contact is judged once, at its first SYN_REPORT. Complete edge grips
 * are dropped for their whole lifetime; every other contact is passed through
 * unchanged. There is no delayed DOWN, synthetic CANCEL, or screen lifecycle.
 * Any stream/write error is fatal: closing the descriptors restores native touch.
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
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <linux/input.h>
#include <linux/uinput.h>

#define MAX_SLOTS 16
#define MAX_FRAME 1024
#define OUT_CAP (MAX_FRAME + 3)
#define NLONGS(n) (((n) + (int)(8 * sizeof(long)) - 1) / (int)(8 * sizeof(long)))
#define BIT_TEST(bits, n) (((bits)[(n) / (8 * sizeof(long))] >> ((n) % (8 * sizeof(long)))) & 1UL)

#define SEEN_X 1
#define SEEN_Y 2
#define SEEN_MAJOR 4

static volatile sig_atomic_t g_stop;
static int src_fd = -1, ui_fd = -1, lock_fd = -1;
static pid_t monitor_pid = -1;

static char device_name[64];
static struct input_id device_id;
static int max_x, max_y;
static int band_l, band_r, band_t, band_b;
static int major_min = 40;
static double scale = 10.0;
static int rot = 1;
static int has_btn_touch, has_tool_finger;

struct slot {
    int active;
    int decided;
    int blocked;
    int x, y, major;
    unsigned seen;
};

struct fev {
    struct input_event ev;
    int slot;                  /* -1: not slot-specific */
    int button;                /* BTN_TOUCH / BTN_TOOL_FINGER */
};

static struct slot slots[MAX_SLOTS];
static int new_slots[MAX_SLOTS], new_count;
static struct fev frame[MAX_FRAME];
static int frame_count, cur_slot;
static int emitted_down;

static void logmsg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[edgefilter] ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    fflush(stderr);
}

static void on_stop(int sig)
{
    (void)sig;
    g_stop = 1;
}

static int in_band(int x, int y)
{
    double l = band_l * scale, r = band_r * scale;
    double t = band_t * scale, b = band_b * scale;

    switch (rot) {
    case 0:
        return (l > 0 && x < l) || (r > 0 && x > max_x - r) ||
               (t > 0 && y < t) || (b > 0 && y > max_y - b);
    case 1:
        return (l > 0 && y < l) || (r > 0 && y > max_y - r) ||
               (t > 0 && x > max_x - t) || (b > 0 && x < b);
    case 2:
        return (l > 0 && x > max_x - l) || (r > 0 && x < r) ||
               (t > 0 && y > max_y - t) || (b > 0 && y < b);
    default:
        return (l > 0 && y > max_y - l) || (r > 0 && y < r) ||
               (t > 0 && x < t) || (b > 0 && x > max_x - b);
    }
}

static int add_event(const struct input_event *ev, int slot, int button)
{
    if (frame_count >= MAX_FRAME) {
        logmsg("input frame overflow; disabling filter");
        return -1;
    }
    frame[frame_count].ev = *ev;
    frame[frame_count].slot = slot;
    frame[frame_count].button = button;
    frame_count++;
    return 0;
}

/* Returns 0 on success. A malformed/dropped source frame is deliberately fatal. */
static int handle_event(const struct input_event *ev)
{
    if (ev->type == EV_SYN) {
        if (ev->code == SYN_DROPPED) {
            logmsg("source SYN_DROPPED; disabling filter instead of guessing state");
            return -1;
        }
        return add_event(ev, -1, 0);
    }

    if (ev->type == EV_KEY) {
        int button = (ev->code == BTN_TOUCH || ev->code == BTN_TOOL_FINGER);
        return add_event(ev, -1, button);
    }

    if (ev->type != EV_ABS)
        return add_event(ev, -1, 0);

    if (ev->code == ABS_X || ev->code == ABS_Y) {
        /* Global legacy axes may describe a rejected MT contact. */
        return add_event(ev, -2, 0);
    }

    if (ev->code == ABS_MT_SLOT) {
        if (ev->value < 0 || ev->value >= MAX_SLOTS) {
            logmsg("invalid source slot %d; disabling filter", ev->value);
            return -1;
        }
        cur_slot = ev->value;
        /* Keep slot selections: omitting them would change subsequent event routing. */
        return add_event(ev, -1, 0);
    }

    switch (ev->code) {
    case ABS_MT_TRACKING_ID:
    case ABS_MT_POSITION_X:
    case ABS_MT_POSITION_Y:
    case ABS_MT_TOUCH_MAJOR:
    case ABS_MT_WIDTH_MAJOR:
    case ABS_MT_WIDTH_MINOR:
        break;
    default:
        return add_event(ev, cur_slot, 0);
    }

    struct slot *s = &slots[cur_slot];
    switch (ev->code) {
    case ABS_MT_TRACKING_ID:
        if (ev->value >= 0) {
            if (s->active || !s->decided) {
                logmsg("invalid DOWN on slot %d; disabling filter", cur_slot);
                return -1;
            }
            if (new_count >= MAX_SLOTS) return -1;
            s->active = 1;
            s->decided = 0;   /* also marks an unjudged DOWN in this frame */
            s->blocked = 0;
            s->x = s->y = s->major = -1;
            s->seen = 0;
            new_slots[new_count++] = cur_slot;
        } else {
            s->active = 0;
        }
        break;
    case ABS_MT_POSITION_X:
        if (!s->decided) { s->x = ev->value; s->seen |= SEEN_X; }
        break;
    case ABS_MT_POSITION_Y:
        if (!s->decided) { s->y = ev->value; s->seen |= SEEN_Y; }
        break;
    case ABS_MT_TOUCH_MAJOR:
        if (!s->decided) { s->major = ev->value; s->seen |= SEEN_MAJOR; }
        break;
    }
    return add_event(ev, cur_slot, 0);
}

static int any_allowed_down(void)
{
    for (int i = 0; i < MAX_SLOTS; i++)
        if (slots[i].active && !slots[i].blocked) return 1;
    return 0;
}

static void decide_new_contacts(void)
{
    for (int n = 0; n < new_count; n++) {
        struct slot *s = &slots[new_slots[n]];
        int complete = (s->seen & (SEEN_X | SEEN_Y)) == (SEEN_X | SEEN_Y);
        int edge = complete && in_band(s->x, s->y);
        int area_known = (s->seen & SEEN_MAJOR) != 0;

        /* Fail open: never judge with a stale axis or an assumed contact area. */
        s->blocked = edge && (major_min == 0 || (area_known && s->major >= major_min));
        s->decided = 1;

        const char *result = s->blocked ? "BLOCKED" : "pass";
        const char *reason = !complete || (edge && major_min > 0 && !area_known)
                           ? "incomplete" : s->blocked ? "grip" : edge ? "area" : "outside";
        logmsg("slot %d new contact at (%d,%d) major=%d -> %s %s",
               new_slots[n], s->x, s->y, s->major, result, reason);
    }
    new_count = 0;
}

static void add_button(struct input_event *out, int *out_count,
                       const struct input_event *stamp, int code, int value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.time = stamp->time;
    ev.type = EV_KEY;
    ev.code = code;
    ev.value = value;
    out[(*out_count)++] = ev;
}

/*
 * Build exactly one output frame. With no rejected contact this is a byte-for-
 * byte copy. Otherwise only rejected slots and global touch buttons are masked;
 * the original event order, coordinates and timestamps of allowed slots remain.
 */
static int finish_frame(const struct input_event *syn,
                        struct input_event *out, int *out_count)
{
    int n = 0, filtered = 0;
    decide_new_contacts();

    for (int i = 0; i < MAX_SLOTS; i++)
        if (slots[i].active && slots[i].blocked) filtered = 1;
    for (int i = 0; i < frame_count; i++)
        if (frame[i].slot >= 0 && slots[frame[i].slot].blocked) filtered = 1;

    if (!filtered) {
        for (int i = 0; i < frame_count; i++) out[n++] = frame[i].ev;
        emitted_down = any_allowed_down();
    } else {
        for (int i = 0; i < frame_count; i++) {
            if (frame[i].button || frame[i].slot == -2) continue;
            if (frame[i].slot >= 0 && slots[frame[i].slot].blocked) continue;
            out[n++] = frame[i].ev;
        }
        int down = any_allowed_down();
        if (down != emitted_down) {
            if (has_btn_touch)
                add_button(out, &n, syn, BTN_TOUCH, down);
            if (has_tool_finger)
                add_button(out, &n, syn, BTN_TOOL_FINGER, down);
            emitted_down = down;
        }
    }
    out[n++] = *syn;
    *out_count = n;
    frame_count = 0;
    return 0;
}

/* uinput writes must never silently discard a partial frame. */
static int write_all(int fd, const void *data, size_t bytes)
{
    const char *p = data;
    while (bytes) {
        ssize_t n = write(fd, p, bytes);
        if (n > 0) { p += n; bytes -= (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n == 0) errno = EIO;
        return -1;
    }
    return 0;
}

static int read_abs(int fd, int code, struct input_absinfo *ai)
{
    memset(ai, 0, sizeof(*ai));
    return ioctl(fd, EVIOCGABS(code), ai);
}

/* Align both current-slot cursors at an empty-contact boundary. */
static int align_current_slot(void)
{
    struct input_absinfo ai;
    struct input_event events[2];
    if (read_abs(src_fd, ABS_MT_SLOT, &ai) < 0 ||
        ai.value < 0 || ai.value >= MAX_SLOTS) return -1;
    cur_slot = ai.value;
    memset(events, 0, sizeof(events));
    events[0].type = EV_ABS;
    events[0].code = ABS_MT_SLOT;
    events[0].value = cur_slot;
    events[1].type = EV_SYN;
    events[1].code = SYN_REPORT;
    return write_all(ui_fd, events, sizeof(events));
}

static int source_touch_down(void)
{
    unsigned long keys[NLONGS(KEY_CNT)];
    memset(keys, 0, sizeof(keys));
    if (ioctl(src_fd, EVIOCGKEY(sizeof(keys)), keys) < 0) return -1;
    return BIT_TEST(keys, BTN_TOUCH) != 0;
}

static int copy_caps(void)
{
    unsigned long keys[NLONGS(KEY_CNT)], abs[NLONGS(ABS_CNT)], props[NLONGS(INPUT_PROP_CNT)];
    memset(keys, 0, sizeof(keys));
    memset(abs, 0, sizeof(abs));
    memset(props, 0, sizeof(props));

    if (ioctl(src_fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0 ||
        ioctl(src_fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs) < 0)
        return -1;
    if (ioctl(ui_fd, UI_SET_EVBIT, EV_SYN) < 0 ||
        ioctl(ui_fd, UI_SET_EVBIT, EV_KEY) < 0 ||
        ioctl(ui_fd, UI_SET_EVBIT, EV_ABS) < 0)
        return -1;

    for (int c = 0; c < KEY_CNT; c++) {
        if (!BIT_TEST(keys, c)) continue;
        if (ioctl(ui_fd, UI_SET_KEYBIT, c) < 0) return -1;
    }
    for (int c = 0; c < ABS_CNT; c++) {
        if (!BIT_TEST(abs, c)) continue;
        struct input_absinfo ai;
        if (read_abs(src_fd, c, &ai) < 0 || ioctl(ui_fd, UI_SET_ABSBIT, c) < 0)
            return -1;
#ifdef UI_ABS_SETUP
        struct uinput_abs_setup setup;
        memset(&setup, 0, sizeof(setup));
        setup.code = c;
        setup.absinfo = ai;
        if (ioctl(ui_fd, UI_ABS_SETUP, &setup) < 0) return -1;
#endif
    }
    if (ioctl(src_fd, EVIOCGPROP(sizeof(props)), props) == 0) {
        for (int c = 0; c < INPUT_PROP_CNT; c++)
            if (BIT_TEST(props, c) && ioctl(ui_fd, UI_SET_PROPBIT, c) < 0) return -1;
    }
    if (ioctl(ui_fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT) < 0) return -1;
    has_btn_touch = BIT_TEST(keys, BTN_TOUCH);
    has_tool_finger = BIT_TEST(keys, BTN_TOOL_FINGER);
    return 0;
}

static int setup_uinput(void)
{
    struct uinput_setup setup;
    ui_fd = open("/dev/uinput", O_WRONLY | O_CLOEXEC);
    if (ui_fd < 0) return -1;
    if (copy_caps() < 0) return -1;

    memset(&setup, 0, sizeof(setup));
    snprintf(setup.name, sizeof(setup.name), "%s-filtered", device_name);
    setup.id = device_id;
    if (ioctl(ui_fd, UI_DEV_SETUP, &setup) < 0 || ioctl(ui_fd, UI_DEV_CREATE) < 0)
        return -1;
    return 0;
}

static int find_device(const char *wanted, char *path, size_t path_size)
{
    DIR *dir = opendir("/dev/input");
    if (!dir) return -1;
    struct dirent *entry;
    int found = -1;
    while ((entry = readdir(dir))) {
        if (strncmp(entry->d_name, "event", 5)) continue;
        snprintf(path, path_size, "/dev/input/%s", entry->d_name);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        char name[128] = {0};
        if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 && !strcmp(name, wanted)) {
            found = 0;
            close(fd);
            break;
        }
        close(fd);
    }
    closedir(dir);
    return found;
}

static void write_info(void)
{
    const char *path = "/data/adb/edgefilter/info";
    const char *tmp = "/data/adb/edgefilter/info.tmp";
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "%d %d %d %s\n", max_x, max_y, rot * 90, device_name);
    fclose(f);
    rename(tmp, path);
}

static void publish_ready(void)
{
    const char *path = "/data/adb/edgefilter/ready";
    const char *tmp = "/data/adb/edgefilter/ready.tmp";
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fputs("1\n", f);
    fclose(f);
    rename(tmp, path);
}

static int detect_rotation(void)
{
    FILE *p = popen("dumpsys input 2>/dev/null", "r");
    if (!p) return -1;
    char line[512];
    int result = -1;
    while (fgets(line, sizeof(line), p)) {
        if (!strstr(line, "Viewport INTERNAL")) continue;
        char *q = strstr(line, "orientation=");
        if (q) {
            int value = atoi(q + 12);
            if (value >= 0 && value <= 3) result = value;
        }
        break;
    }
    pclose(p);
    return result;
}

/* dumpsys can take tens of milliseconds, so it never runs in the input loop. */
static pid_t start_rotation_monitor(int *read_fd)
{
    int fds[2];
    if (pipe(fds) < 0) return -1;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);

    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return -1; }
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        close(fds[0]);
        if (src_fd >= 0) close(src_fd);
        if (ui_fd >= 0) close(ui_fd);
        if (lock_fd >= 0) close(lock_fd);
        int last = rot;
        for (;;) {
            sleep(2);
            int value = detect_rotation();
            if (value < 0 || value == last) continue;
            last = value;
            unsigned char byte = (unsigned char)value;
            if (write(fds[1], &byte, 1) != 1) _exit(0);
        }
    }
    close(fds[1]);
    *read_fd = fds[0];
    return pid;
}

static void cleanup(void)
{
    if (monitor_pid > 0) {
        kill(monitor_pid, SIGTERM);
        waitpid(monitor_pid, NULL, 0);
        monitor_pid = -1;
    }
    if (ui_fd >= 0) {
        ioctl(ui_fd, UI_DEV_DESTROY);
        close(ui_fd);
        ui_fd = -1;
    }
    if (src_fd >= 0) {
        ioctl(src_fd, EVIOCGRAB, 0);
        close(src_fd);
        src_fd = -1;
    }
    if (lock_fd >= 0) {
        unlink("/data/adb/edgefilter/ready");
        close(lock_fd);
        lock_fd = -1;
    }
}

static void usage(void)
{
    fprintf(stderr,
        "usage: edgefilter [--name NAME|-d DEV] [--left N] [--right N]\n"
        "                  [--top N] [--bottom N] [--major N] [--scale F]\n"
        "                  [--rot auto|0|90|180|270] [--info]\n"
        "Complete contacts in an edge band with major >= --major are dropped.\n"
        "Incomplete contacts pass unchanged; --major 0 drops every complete edge contact.\n");
}

int main(int argc, char **argv)
{
    const char *dev = NULL, *name = "goodix_ts";
    char path[256] = {0};
    int info_only = 0, rot_auto = 1, status = 1;

    /* Drop descriptors inherited from the launcher. ctl.sh runs under a file
     * lock, and leaking that descriptor into this long-lived process would
     * keep the lock held forever. */
    for (int fd = 3; fd < 256; fd++) close(fd);

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc) dev = argv[++i];
        else if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(argv[i], "--left") && i + 1 < argc) band_l = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--right") && i + 1 < argc) band_r = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--top") && i + 1 < argc) band_t = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bottom") && i + 1 < argc) band_b = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--major") && i + 1 < argc) major_min = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atof(argv[++i]);
        else if (!strcmp(argv[i], "--info")) info_only = 1;
        else if (!strcmp(argv[i], "--rot") && i + 1 < argc) {
            const char *v = argv[++i];
            if (strcmp(v, "auto")) { rot = atoi(v) / 90; rot_auto = 0; }
        } else { usage(); return 2; }
    }
    if (band_l < 0 || band_r < 0 || band_t < 0 || band_b < 0 ||
        major_min < 0 || scale <= 0 || rot < 0 || rot > 3) {
        usage();
        return 2;
    }

    lock_fd = open("/data/adb/edgefilter/daemon.lock", O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock_fd >= 0 && flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
        close(lock_fd);
        lock_fd = -1;
    }
    if (lock_fd < 0) {
        logmsg("another edgefilter instance is already active");
        goto out;
    }
    unlink("/data/adb/edgefilter/ready");

    if (!dev) {
        if (find_device(name, path, sizeof(path)) < 0) {
            logmsg("no input device named '%s'", name);
            return 1;
        }
        dev = path;
    }
    src_fd = open(dev, O_RDONLY | O_CLOEXEC);
    if (src_fd < 0) { logmsg("open %s failed: %s", dev, strerror(errno)); return 1; }
    if (ioctl(src_fd, EVIOCGNAME(sizeof(device_name)), device_name) < 0 ||
        ioctl(src_fd, EVIOCGID, &device_id) < 0) {
        logmsg("cannot read source identity: %s", strerror(errno));
        goto out;
    }

    struct input_absinfo aix = {0}, aiy = {0}, ais = {0};
    if (read_abs(src_fd, ABS_MT_POSITION_X, &aix) < 0 ||
        read_abs(src_fd, ABS_MT_POSITION_Y, &aiy) < 0 ||
        read_abs(src_fd, ABS_MT_SLOT, &ais) < 0 || ais.maximum >= MAX_SLOTS) {
        logmsg("unsupported MT device (slots=%d)", ais.maximum + 1);
        goto out;
    }
    max_x = aix.maximum;
    max_y = aiy.maximum;
    if (rot_auto) {
        int value = detect_rotation();
        if (value >= 0) rot = value;
    }
    write_info();
    logmsg("source %s (%s) panel 0..%d x 0..%d rot=%d scale=%.1f",
           dev, device_name, max_x, max_y, rot * 90, scale);
    logmsg("bands L=%d R=%d T=%d B=%d px major_min=%d; incomplete=pass; persistent=uinput",
           band_l, band_r, band_t, band_b, major_min);
    if (info_only) { status = 0; goto out; }

    for (int i = 0; i < MAX_SLOTS; i++) slots[i].decided = 1;
    if (setup_uinput() < 0) {
        logmsg("uinput setup failed: %s", strerror(errno));
        goto out;
    }
    usleep(100000);             /* let InputReader open the device before grabbing */
    if (align_current_slot() < 0) {
        logmsg("cannot align source/virtual slots: %s", strerror(errno));
        goto out;
    }
    int native_down = source_touch_down();
    if (native_down != 0) {
        logmsg(native_down < 0 ? "cannot query native contacts: %s" :
               "native contact is active; retry after all fingers lift%s",
               native_down < 0 ? strerror(errno) : "");
        goto out;
    }
    if (ioctl(src_fd, EVIOCGRAB, 1) < 0) {
        logmsg("EVIOCGRAB failed: %s", strerror(errno));
        goto out;
    }
    /* Close the query/grab race: a DOWN that began before grab is not ours. */
    native_down = source_touch_down();
    if (native_down != 0) {
        logmsg("contact began during startup; native touch restored");
        goto out;
    }
    publish_ready();
    logmsg("grabbed %s; virtual device '%s-filtered' is persistent", dev, device_name);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_stop;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    /* The touch stream is persistent across screen state, so a stale watcher
     * signalling this PID must not be able to stop filtering. */
    signal(SIGHUP, SIG_IGN);
    signal(SIGUSR1, SIG_IGN);
    signal(SIGUSR2, SIG_IGN);

    int rotation_fd = -1;
    if (rot_auto) monitor_pid = start_rotation_monitor(&rotation_fd);
    struct pollfd pfds[2];
    pfds[0].fd = src_fd;
    pfds[0].events = POLLIN;
    pfds[1].fd = rotation_fd;
    pfds[1].events = POLLIN;

    while (!g_stop) {
        int ready = poll(pfds, 2, -1);
        if (ready < 0) {
            if (errno == EINTR) continue;
            logmsg("poll failed: %s", strerror(errno));
            goto out;
        }
        if (rotation_fd >= 0 && (pfds[1].revents & POLLIN)) {
            unsigned char values[16];
            ssize_t count = read(rotation_fd, values, sizeof(values));
            if (count > 0 && values[count - 1] <= 3 && rot != values[count - 1]) {
                rot = values[count - 1];
                logmsg("rotation changed -> %d", rot * 90);
                write_info();
            }
        }
        if (rotation_fd >= 0 && (pfds[1].revents & (POLLERR | POLLHUP | POLLNVAL))) {
            close(rotation_fd);
            rotation_fd = pfds[1].fd = -1;
        }
        if (!(pfds[0].revents & POLLIN)) {
            if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                logmsg("source device disappeared; disabling filter");
                goto out;
            }
            continue;
        }

        struct input_event input[256];
        ssize_t bytes = read(src_fd, input, sizeof(input));
        if (bytes <= 0) {
            if (bytes < 0 && errno == EINTR) continue;
            logmsg("source read failed: %s", bytes < 0 ? strerror(errno) : "EOF");
            goto out;
        }
        if (bytes % (ssize_t)sizeof(struct input_event)) {
            logmsg("partial input_event read; disabling filter");
            goto out;
        }
        int count = (int)(bytes / (ssize_t)sizeof(struct input_event));
        for (int i = 0; i < count; i++) {
            if (input[i].type == EV_SYN && input[i].code == SYN_REPORT) {
                struct input_event output[OUT_CAP];
                int output_count = 0;
                finish_frame(&input[i], output, &output_count);
                if (write_all(ui_fd, output, (size_t)output_count * sizeof(output[0])) < 0) {
                    logmsg("uinput write failed: %s; native touch restored", strerror(errno));
                    goto out;
                }
            } else if (handle_event(&input[i]) < 0) {
                goto out;
            }
        }
    }
    status = 0;

out:
    logmsg("exiting; native touch restored");
    cleanup();
    return status;
}
