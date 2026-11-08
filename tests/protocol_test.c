/*
 * Host-side protocol regression tests for the EdgeFilter state machine.
 *
 * The daemon is a single translation unit, so the test includes it with main()
 * renamed and drives handle_event()/finish_frame() directly. No uinput, no
 * device, no root: every assertion is about the event stream we would emit.
 *
 *   cc -std=gnu11 -Wall -Wextra -Wno-unused-function -o tests/protocol_test tests/protocol_test.c
 */
#define main edgefilter_main
#include "../src/edgefilter.c"
#undef main

static int failures;
static int checks;

#define CHECK(cond, ...) do {                                   \
    checks++;                                                   \
    if (!(cond)) {                                              \
        failures++;                                             \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);    \
        fprintf(stderr, __VA_ARGS__);                           \
        fputc('\n', stderr);                                    \
    }                                                           \
} while (0)

static struct input_event out[OUT_CAP];
static int out_n;

static void reset_for_test(void)
{
    memset(slots, 0, sizeof(slots));
    for (int i = 0; i < MAX_SLOTS; i++) slots[i].decided = 1;
    new_count = frame_count = cur_slot = 0;
    emitted_down = 0;
    out_n = 0;
}

static void set_geometry(void)
{
    max_x = 21359;
    max_y = 31999;
    scale = 10.0;
    rot = 1;                    /* landscape: left = small y, top = large x */
    band_l = band_r = 150;
    band_t = band_b = 0;
    major_min = 40;
    has_btn_touch = 1;
    has_tool_finger = 1;
}

static struct input_event ev(int type, int code, int value, int sec, int usec)
{
    struct input_event e;
    memset(&e, 0, sizeof(e));
    e.time.tv_sec = sec;
    e.time.tv_usec = usec;
    e.type = type;
    e.code = code;
    e.value = value;
    return e;
}

#define FRAME(f) feed((f), (int)(sizeof(f) / sizeof((f)[0])))
#define NF(f)    ((int)(sizeof(f) / sizeof((f)[0])))

/* Feed exactly one frame; the trailing SYN_REPORT produces the output frame. */
static void feed(const struct input_event *in, int n)
{
    out_n = 0;
    for (int i = 0; i < n; i++) {
        if (in[i].type == EV_SYN && in[i].code == SYN_REPORT)
            CHECK(finish_frame(&in[i], out, &out_n) == 0, "finish_frame failed");
        else
            CHECK(handle_event(&in[i]) == 0, "handle_event rejected frame");
    }
}

/* count == 0 -> count every event with this type/code, ignoring the value */
static int count(int type, int code, int value, int exact)
{
    int n = 0;
    for (int i = 0; i < out_n; i++)
        if (out[i].type == type && out[i].code == code &&
            (!exact || out[i].value == value)) n++;
    return n;
}

static int identical(const struct input_event *in, int n)
{
    return out_n == n && memcmp(in, out, (size_t)n * sizeof(out[0])) == 0;
}

/* A frame that carries no contact payload must still end with one SYN_REPORT. */
static void check_no_contact_payload(const char *what)
{
    CHECK(count(EV_ABS, ABS_MT_TRACKING_ID, 0, 0) == 0, "%s: TRACKING_ID leaked", what);
    CHECK(count(EV_ABS, ABS_MT_POSITION_X, 0, 0) == 0, "%s: POSITION_X leaked", what);
    CHECK(count(EV_ABS, ABS_MT_POSITION_Y, 0, 0) == 0, "%s: POSITION_Y leaked", what);
    CHECK(count(EV_ABS, ABS_MT_TOUCH_MAJOR, 0, 0) == 0, "%s: TOUCH_MAJOR leaked", what);
    CHECK(count(EV_KEY, BTN_TOUCH, 0, 0) == 0, "%s: BTN_TOUCH leaked", what);
    CHECK(count(EV_KEY, BTN_TOOL_FINGER, 0, 0) == 0, "%s: BTN_TOOL_FINGER leaked", what);
    CHECK(count(EV_SYN, SYN_REPORT, 0, 0) == 1, "%s: must end with exactly one SYN_REPORT", what);
}

/* ---- 1. a contact that is not filtered is copied byte-for-byte ---- */
static void test_identity_passthrough(void)
{
    reset_for_test();
    struct input_event f[] = {
        ev(EV_ABS, ABS_MT_SLOT, 0, 1, 0),
        ev(EV_ABS, ABS_MT_TRACKING_ID, 100, 1, 0),
        ev(EV_ABS, ABS_MT_POSITION_X, 10000, 1, 0),
        ev(EV_ABS, ABS_MT_POSITION_Y, 16000, 1, 0),
        ev(EV_ABS, ABS_MT_TOUCH_MAJOR, 25, 1, 0),
        ev(EV_KEY, BTN_TOUCH, 1, 1, 0),
        ev(EV_KEY, BTN_TOOL_FINGER, 1, 1, 0),
        ev(EV_SYN, SYN_REPORT, 0, 1, 0),
    };
    FRAME(f);
    CHECK(identical(f, NF(f)), "unfiltered contact was rewritten (%d events out)", out_n);
}

/* ---- 2. a complete edge grip is dropped for its whole lifetime ---- */
static void test_edge_grip_dropped(void)
{
    reset_for_test();
    struct input_event down[] = {
        ev(EV_ABS, ABS_MT_SLOT, 0, 2, 0),
        ev(EV_ABS, ABS_MT_TRACKING_ID, 200, 2, 0),
        ev(EV_ABS, ABS_MT_POSITION_X, 10000, 2, 0),
        ev(EV_ABS, ABS_MT_POSITION_Y, 500, 2, 0),        /* left band */
        ev(EV_ABS, ABS_MT_TOUCH_MAJOR, 60, 2, 0),
        ev(EV_KEY, BTN_TOUCH, 1, 2, 0),
        ev(EV_KEY, BTN_TOOL_FINGER, 1, 2, 0),
        ev(EV_SYN, SYN_REPORT, 0, 2, 0),
    };
    FRAME(down);
    check_no_contact_payload("blocked DOWN");

    struct input_event move[] = {
        ev(EV_ABS, ABS_MT_POSITION_X, 10100, 3, 0),
        ev(EV_ABS, ABS_MT_POSITION_Y, 600, 3, 0),
        ev(EV_SYN, SYN_REPORT, 0, 3, 0),
    };
    FRAME(move);
    check_no_contact_payload("blocked move");

    struct input_event up[] = {
        ev(EV_ABS, ABS_MT_SLOT, 0, 4, 0),
        ev(EV_ABS, ABS_MT_TRACKING_ID, -1, 4, 0),
        ev(EV_KEY, BTN_TOUCH, 0, 4, 0),
        ev(EV_SYN, SYN_REPORT, 0, 4, 0),
    };
    FRAME(up);
    check_no_contact_payload("blocked UP");
}

/* ---- 3. the same edge position passes when the area is a fingertip ---- */
static void test_edge_fingertip_passes(void)
{
    reset_for_test();
    struct input_event f[] = {
        ev(EV_ABS, ABS_MT_SLOT, 0, 5, 0),
        ev(EV_ABS, ABS_MT_TRACKING_ID, 300, 5, 0),
        ev(EV_ABS, ABS_MT_POSITION_X, 10000, 5, 0),
        ev(EV_ABS, ABS_MT_POSITION_Y, 500, 5, 0),
        ev(EV_ABS, ABS_MT_TOUCH_MAJOR, 20, 5, 0),
        ev(EV_KEY, BTN_TOUCH, 1, 5, 0),
        ev(EV_SYN, SYN_REPORT, 0, 5, 0),
    };
    FRAME(f);
    CHECK(identical(f, NF(f)), "edge fingertip was filtered");
}

/* ---- 4. an incomplete first frame fails open, never guesses ---- */
static void test_incomplete_fails_open(void)
{
    reset_for_test();
    struct input_event no_y[] = {
        ev(EV_ABS, ABS_MT_SLOT, 0, 6, 0),
        ev(EV_ABS, ABS_MT_TRACKING_ID, 400, 6, 0),
        ev(EV_ABS, ABS_MT_POSITION_X, 10000, 6, 0),   /* Y missing */
        ev(EV_ABS, ABS_MT_TOUCH_MAJOR, 90, 6, 0),
        ev(EV_SYN, SYN_REPORT, 0, 6, 0),
    };
    FRAME(no_y);
    CHECK(identical(no_y, NF(no_y)), "incomplete frame was dropped");

    reset_for_test();
    struct input_event no_major[] = {
        ev(EV_ABS, ABS_MT_SLOT, 0, 7, 0),
        ev(EV_ABS, ABS_MT_TRACKING_ID, 401, 7, 0),
        ev(EV_ABS, ABS_MT_POSITION_X, 10000, 7, 0),
        ev(EV_ABS, ABS_MT_POSITION_Y, 500, 7, 0),
        ev(EV_SYN, SYN_REPORT, 0, 7, 0),
    };
    FRAME(no_major);
    CHECK(identical(no_major, NF(no_major)), "missing area was assumed blocked");
}

/* ---- 5. a blocked finger must not disturb a coexisting allowed finger ---- */
static void test_mixed_contacts(void)
{
    reset_for_test();
    struct input_event both[] = {
        ev(EV_ABS, ABS_MT_SLOT, 0, 8, 0),
        ev(EV_ABS, ABS_MT_TRACKING_ID, 500, 8, 0),
        ev(EV_ABS, ABS_MT_POSITION_X, 10000, 8, 0),
        ev(EV_ABS, ABS_MT_POSITION_Y, 16000, 8, 0),
        ev(EV_ABS, ABS_MT_TOUCH_MAJOR, 25, 8, 0),
        ev(EV_KEY, BTN_TOUCH, 1, 8, 0),
        ev(EV_KEY, BTN_TOOL_FINGER, 1, 8, 0),
        ev(EV_ABS, ABS_MT_SLOT, 1, 8, 0),
        ev(EV_ABS, ABS_MT_TRACKING_ID, 501, 8, 0),
        ev(EV_ABS, ABS_MT_POSITION_X, 12000, 8, 0),
        ev(EV_ABS, ABS_MT_POSITION_Y, 300, 8, 0),
        ev(EV_ABS, ABS_MT_TOUCH_MAJOR, 70, 8, 0),
        ev(EV_SYN, SYN_REPORT, 0, 8, 0),
    };
    FRAME(both);
    CHECK(count(EV_ABS, ABS_MT_TRACKING_ID, 500, 1) == 1, "allowed DOWN lost");
    CHECK(count(EV_ABS, ABS_MT_TRACKING_ID, 501, 1) == 0, "blocked DOWN leaked");
    CHECK(count(EV_ABS, ABS_MT_POSITION_Y, 300, 1) == 0, "blocked coordinates leaked");
    CHECK(count(EV_ABS, ABS_MT_POSITION_Y, 16000, 1) == 1, "allowed coordinates lost");
    CHECK(count(EV_KEY, BTN_TOUCH, 1, 1) == 1, "BTN_TOUCH=1 must appear exactly once");
    CHECK(count(EV_KEY, BTN_TOOL_FINGER, 1, 1) == 1, "BTN_TOOL_FINGER=1 must appear exactly once");
    CHECK(count(EV_SYN, SYN_REPORT, 0, 0) == 1, "frame must end with one SYN_REPORT");

    /* the allowed finger moves while the grip stays down: motion must survive */
    struct input_event move[] = {
        ev(EV_ABS, ABS_MT_SLOT, 0, 9, 0),
        ev(EV_ABS, ABS_MT_POSITION_X, 10500, 9, 0),
        ev(EV_ABS, ABS_MT_POSITION_Y, 16500, 9, 0),
        ev(EV_SYN, SYN_REPORT, 0, 9, 0),
    };
    FRAME(move);
    CHECK(count(EV_ABS, ABS_MT_POSITION_X, 10500, 1) == 1, "allowed motion lost");
    CHECK(count(EV_KEY, BTN_TOUCH, 0, 0) == 0, "BTN_TOUCH must not change during motion");
}

/* ---- 6. BTN_TOUCH must follow allowed contacts only ---- */
static void test_button_follows_allowed_contacts(void)
{
    reset_for_test();
    struct input_event both[] = {
        ev(EV_ABS, ABS_MT_SLOT, 0, 10, 0),
        ev(EV_ABS, ABS_MT_TRACKING_ID, 600, 10, 0),
        ev(EV_ABS, ABS_MT_POSITION_X, 10000, 10, 0),
        ev(EV_ABS, ABS_MT_POSITION_Y, 16000, 10, 0),
        ev(EV_ABS, ABS_MT_TOUCH_MAJOR, 25, 10, 0),
        ev(EV_KEY, BTN_TOUCH, 1, 10, 0),
        ev(EV_ABS, ABS_MT_SLOT, 1, 10, 0),
        ev(EV_ABS, ABS_MT_TRACKING_ID, 601, 10, 0),
        ev(EV_ABS, ABS_MT_POSITION_X, 12000, 10, 0),
        ev(EV_ABS, ABS_MT_POSITION_Y, 300, 10, 0),
        ev(EV_ABS, ABS_MT_TOUCH_MAJOR, 70, 10, 0),
        ev(EV_SYN, SYN_REPORT, 0, 10, 0),
    };
    FRAME(both);
    CHECK(count(EV_KEY, BTN_TOUCH, 1, 1) == 1, "BTN_TOUCH=1 missing while a finger is allowed");

    /* lift the allowed finger; the blocked one is still physically down */
    struct input_event lift[] = {
        ev(EV_ABS, ABS_MT_SLOT, 0, 11, 0),
        ev(EV_ABS, ABS_MT_TRACKING_ID, -1, 11, 0),
        ev(EV_KEY, BTN_TOUCH, 0, 11, 0),
        ev(EV_SYN, SYN_REPORT, 0, 11, 0),
    };
    FRAME(lift);
    CHECK(count(EV_ABS, ABS_MT_TRACKING_ID, -1, 1) == 1, "allowed UP was filtered");
    CHECK(count(EV_KEY, BTN_TOUCH, 0, 1) == 1, "BTN_TOUCH=0 not forwarded");
    CHECK(count(EV_KEY, BTN_TOOL_FINGER, 0, 1) == 1, "BTN_TOOL_FINGER=0 not forwarded");

    /* the blocked finger lifting must not produce a second release */
    struct input_event blocked_lift[] = {
        ev(EV_ABS, ABS_MT_SLOT, 1, 12, 0),
        ev(EV_ABS, ABS_MT_TRACKING_ID, -1, 12, 0),
        ev(EV_KEY, BTN_TOUCH, 0, 12, 0),
        ev(EV_SYN, SYN_REPORT, 0, 12, 0),
    };
    FRAME(blocked_lift);
    CHECK(count(EV_KEY, BTN_TOUCH, 0, 0) == 0, "duplicate BTN_TOUCH release");
    CHECK(count(EV_ABS, ABS_MT_TRACKING_ID, 0, 0) == 0, "blocked UP leaked");
}

/* ---- 7. every allowed contact gets exactly one DOWN and one UP ---- */
static void test_no_duplicate_tracking_ids(void)
{
    reset_for_test();
    struct input_event down[] = {
        ev(EV_ABS, ABS_MT_SLOT, 0, 13, 0),
        ev(EV_ABS, ABS_MT_TRACKING_ID, 700, 13, 0),
        ev(EV_ABS, ABS_MT_POSITION_X, 10000, 13, 0),
        ev(EV_ABS, ABS_MT_POSITION_Y, 16000, 13, 0),
        ev(EV_ABS, ABS_MT_TOUCH_MAJOR, 25, 13, 0),
        ev(EV_SYN, SYN_REPORT, 0, 13, 0),
    };
    FRAME(down);
    CHECK(count(EV_ABS, ABS_MT_TRACKING_ID, 700, 1) == 1, "DOWN count != 1");
    CHECK(count(EV_SYN, SYN_REPORT, 0, 0) == 1, "DOWN frame must end with one SYN_REPORT");

    struct input_event up[] = {
        ev(EV_ABS, ABS_MT_TRACKING_ID, -1, 14, 0),
        ev(EV_SYN, SYN_REPORT, 0, 14, 0),
    };
    FRAME(up);
    CHECK(count(EV_ABS, ABS_MT_TRACKING_ID, -1, 1) == 1, "UP count != 1");
    CHECK(count(EV_SYN, SYN_REPORT, 0, 0) == 1, "UP frame must end with one SYN_REPORT");
}

/* ---- 8. legacy axes are masked only while a contact is filtered ---- */
static void test_legacy_axes(void)
{
    reset_for_test();
    struct input_event clean[] = {
        ev(EV_ABS, ABS_MT_SLOT, 0, 15, 0),
        ev(EV_ABS, ABS_MT_TRACKING_ID, 800, 15, 0),
        ev(EV_ABS, ABS_MT_POSITION_X, 10000, 15, 0),
        ev(EV_ABS, ABS_MT_POSITION_Y, 16000, 15, 0),
        ev(EV_ABS, ABS_MT_TOUCH_MAJOR, 25, 15, 0),
        ev(EV_ABS, ABS_X, 10000, 15, 0),
        ev(EV_ABS, ABS_Y, 16000, 15, 0),
        ev(EV_SYN, SYN_REPORT, 0, 15, 0),
    };
    FRAME(clean);
    CHECK(identical(clean, NF(clean)), "legacy axes rewritten without filtering");

    reset_for_test();
    struct input_event mixed[] = {
        ev(EV_ABS, ABS_MT_SLOT, 0, 16, 0),
        ev(EV_ABS, ABS_MT_TRACKING_ID, 801, 16, 0),
        ev(EV_ABS, ABS_MT_POSITION_X, 10000, 16, 0),
        ev(EV_ABS, ABS_MT_POSITION_Y, 16000, 16, 0),
        ev(EV_ABS, ABS_MT_TOUCH_MAJOR, 25, 16, 0),
        ev(EV_ABS, ABS_MT_SLOT, 1, 16, 0),
        ev(EV_ABS, ABS_MT_TRACKING_ID, 802, 16, 0),
        ev(EV_ABS, ABS_MT_POSITION_X, 12000, 16, 0),
        ev(EV_ABS, ABS_MT_POSITION_Y, 300, 16, 0),
        ev(EV_ABS, ABS_MT_TOUCH_MAJOR, 70, 16, 0),
        ev(EV_ABS, ABS_X, 12000, 16, 0),      /* describes the blocked grip */
        ev(EV_ABS, ABS_Y, 300, 16, 0),
        ev(EV_SYN, SYN_REPORT, 0, 16, 0),
    };
    FRAME(mixed);
    CHECK(count(EV_ABS, ABS_X, 12000, 1) == 0, "blocked legacy X leaked");
    CHECK(count(EV_ABS, ABS_Y, 300, 1) == 0, "blocked legacy Y leaked");
    CHECK(count(EV_ABS, ABS_MT_TRACKING_ID, 801, 1) == 1, "allowed contact lost");
}

/* ---- 9. a source event overflow must stop the filter, not guess ---- */
static void test_syn_dropped_is_fatal(void)
{
    reset_for_test();
    struct input_event drop = ev(EV_SYN, SYN_DROPPED, 0, 17, 0);
    CHECK(handle_event(&drop) != 0, "SYN_DROPPED must abort the filter");
    frame_count = 0;
}

/* ---- 10. rotation-aware band mapping ---- */
static void test_rotation_mapping(void)
{
    rot = 0;                     /* portrait */
    max_x = 21359; max_y = 31999;
    band_l = band_r = 150; band_t = band_b = 0;
    scale = 10.0;
    CHECK(in_band(500, 16000), "portrait left band missed");
    CHECK(in_band(21000, 16000), "portrait right band missed");
    CHECK(!in_band(10000, 16000), "portrait centre wrongly in band");

    rot = 1;
    CHECK(in_band(10000, 500), "landscape left band missed");
    CHECK(in_band(10000, 31000), "landscape right band missed");
    CHECK(!in_band(10000, 16000), "landscape centre wrongly in band");

    /* zero width disables that edge entirely */
    band_l = 0; band_r = 0;
    CHECK(!in_band(10000, 10), "disabled band still filtered");
}

/* ---- 11. a frame larger than MAX_FRAME must abort, not truncate ---- */
static void test_frame_overflow_is_fatal(void)
{
    reset_for_test();
    struct input_event e = ev(EV_ABS, ABS_MT_SLOT, 0, 18, 0);
    int rejected = 0;
    for (int i = 0; i < MAX_FRAME + 1; i++)
        if (handle_event(&e) != 0) rejected = 1;
    CHECK(rejected, "frame overflow was silently accepted");
    frame_count = 0;
}

/*
 * ---- 12. randomised sequences ----
 * Reconstructs the virtual device state purely from the emitted stream and
 * asserts it equals "every active contact that was not rejected". This is the
 * invariant that makes ghost contacts and coordinate drift impossible.
 */
static unsigned rng_state = 12345;
static unsigned rnd(void)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return (rng_state >> 16) & 0x7fff;
}

static void test_randomised_sequences(void)
{
    int gen_down[MAX_SLOTS] = {0};
    int gen_tid[MAX_SLOTS] = {0};
    int next_tid = 1000;
    int source_btn = 0;

    for (unsigned seed = 1; seed <= 40; seed++) {
        rng_state = seed * 7919u;
        reset_for_test();
        memset(gen_down, 0, sizeof(gen_down));
        next_tid = 1000;
        source_btn = 0;

        /* model of the virtual device, rebuilt only from emitted events and
         * carried across frames exactly like the real device state */
        int out_down[MAX_SLOTS] = {0};
        int cursor = 0, btn = 0;

        for (int step = 0; step < 120; step++) {
            struct input_event f[64];
            int n = 0;
            int slot, choice = (int)(rnd() % 100);

            if (choice < 35) {                     /* press a free slot */
                slot = -1;
                for (int i = 0; i < MAX_SLOTS; i++)
                    if (!gen_down[i]) { slot = i; break; }
                if (slot >= 0) {
                    gen_down[slot] = 1;
                    gen_tid[slot] = next_tid++;
                    int x = (int)(rnd() % (unsigned)max_x);
                    int y = (int)(rnd() % (unsigned)max_y);
                    if (rnd() % 2) y = (int)(rnd() % 1600);            /* force left band */
                    else if (rnd() % 2) y = max_y - (int)(rnd() % 1600); /* force right band */
                    int major = (int)(rnd() % 110);
                    f[n++] = ev(EV_ABS, ABS_MT_SLOT, slot, 100, step);
                    f[n++] = ev(EV_ABS, ABS_MT_TRACKING_ID, gen_tid[slot], 100, step);
                    f[n++] = ev(EV_ABS, ABS_MT_POSITION_X, x, 100, step);
                    f[n++] = ev(EV_ABS, ABS_MT_POSITION_Y, y, 100, step);
                    f[n++] = ev(EV_ABS, ABS_MT_TOUCH_MAJOR, major, 100, step);
                }
            } else if (choice < 75) {              /* move a pressed slot */
                for (slot = 0; slot < MAX_SLOTS; slot++)
                    if (gen_down[slot]) break;
                if (slot < MAX_SLOTS) {
                    f[n++] = ev(EV_ABS, ABS_MT_SLOT, slot, 100, step);
                    f[n++] = ev(EV_ABS, ABS_MT_POSITION_X,
                                (int)(rnd() % (unsigned)max_x), 100, step);
                    f[n++] = ev(EV_ABS, ABS_MT_POSITION_Y,
                                (int)(rnd() % (unsigned)max_y), 100, step);
                }
            } else {                               /* release a pressed slot */
                for (slot = 0; slot < MAX_SLOTS; slot++)
                    if (gen_down[slot]) break;
                if (slot < MAX_SLOTS) {
                    gen_down[slot] = 0;
                    f[n++] = ev(EV_ABS, ABS_MT_SLOT, slot, 100, step);
                    f[n++] = ev(EV_ABS, ABS_MT_TRACKING_ID, -1, 100, step);
                }
            }

            int any = 0;
            for (int i = 0; i < MAX_SLOTS; i++) if (gen_down[i]) any = 1;
            if (any != source_btn) {
                source_btn = any;
                f[n++] = ev(EV_KEY, BTN_TOUCH, source_btn, 100, step);
                f[n++] = ev(EV_KEY, BTN_TOOL_FINGER, source_btn, 100, step);
            }
            f[n++] = ev(EV_SYN, SYN_REPORT, 0, 100, step);

            feed(f, n);

            /* exactly one terminator */
            CHECK(out_n > 0 && out[out_n - 1].type == EV_SYN &&
                  out[out_n - 1].code == SYN_REPORT, "seed %u step %d: bad terminator", seed, step);

            /* apply the emitted stream to the virtual-device model */
            for (int i = 0; i < out_n; i++) {
                if (out[i].type == EV_ABS && out[i].code == ABS_MT_SLOT) {
                    cursor = out[i].value;
                } else if (out[i].type == EV_ABS && out[i].code == ABS_MT_TRACKING_ID) {
                    if (out[i].value >= 0) {
                        CHECK(!out_down[cursor], "seed %u step %d: slot %d double DOWN",
                              seed, step, cursor);
                        out_down[cursor] = 1;
                    } else {
                        out_down[cursor] = 0;
                    }
                } else if (out[i].type == EV_KEY && out[i].code == BTN_TOUCH) {
                    btn = out[i].value;
                }
            }

            for (int i = 0; i < MAX_SLOTS; i++) {
                int expected = slots[i].active && !slots[i].blocked;
                CHECK(out_down[i] == expected,
                      "seed %u step %d: slot %d virtual=%d expected=%d",
                      seed, step, i, out_down[i], expected);
            }
            CHECK(btn == emitted_down, "seed %u step %d: BTN_TOUCH=%d expected=%d",
                  seed, step, btn, emitted_down);
        }
    }
}

int main(void)
{
    set_geometry();
    test_identity_passthrough();
    test_edge_grip_dropped();
    test_edge_fingertip_passes();
    test_incomplete_fails_open();
    test_mixed_contacts();
    test_button_follows_allowed_contacts();
    test_no_duplicate_tracking_ids();
    test_legacy_axes();
    test_syn_dropped_is_fatal();
    test_rotation_mapping();
    test_frame_overflow_is_fatal();
    test_randomised_sequences();

    fprintf(stderr, "\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
