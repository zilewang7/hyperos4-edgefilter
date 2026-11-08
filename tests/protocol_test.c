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
    new_count = frame_count = cur_slot = frame_start_slot = 0;
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
    core_l = core_r = 40;       /* inner strip: y < 400 or y > 31599 */
    core_t = core_b = 0;
    core_slop = 12;
    core_hold_ms = 200;
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

#define NF(f) ((int)(sizeof(f) / sizeof((f)[0])))

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
    /* The proxy must never manufacture a device-wide resync. */
    for (int i = 0; i < out_n; i++)
        CHECK(!(out[i].type == EV_SYN && out[i].code == SYN_DROPPED),
              "SYN_DROPPED must never be emitted");
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

static void check_no_contact_payload(const char *what)
{
    CHECK(count(EV_ABS, ABS_MT_TRACKING_ID, 0, 0) == 0, "%s: TRACKING_ID leaked", what);
    CHECK(count(EV_ABS, ABS_MT_POSITION_X, 0, 0) == 0, "%s: POSITION_X leaked", what);
    CHECK(count(EV_ABS, ABS_MT_POSITION_Y, 0, 0) == 0, "%s: POSITION_Y leaked", what);
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
    feed(f, NF(f));
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
        ev(EV_ABS, ABS_MT_POSITION_Y, 500, 2, 0),
        ev(EV_ABS, ABS_MT_TOUCH_MAJOR, 60, 2, 0),
        ev(EV_KEY, BTN_TOUCH, 1, 2, 0),
        ev(EV_KEY, BTN_TOOL_FINGER, 1, 2, 0),
        ev(EV_SYN, SYN_REPORT, 0, 2, 0),
    };
    feed(down, NF(down));
    check_no_contact_payload("blocked DOWN");

    struct input_event up[] = {
        ev(EV_ABS, ABS_MT_SLOT, 0, 4, 0),
        ev(EV_ABS, ABS_MT_TRACKING_ID, -1, 4, 0),
        ev(EV_KEY, BTN_TOUCH, 0, 4, 0),
        ev(EV_SYN, SYN_REPORT, 0, 4, 0),
    };
    feed(up, NF(up));
    check_no_contact_payload("blocked UP");
}

/* ---- 3. a fingertip inside the band but outside the core strip passes ---- */
static void test_band_fingertip_passes(void)
{
    reset_for_test();
    struct input_event f[] = {
        ev(EV_ABS, ABS_MT_SLOT, 0, 5, 0),
        ev(EV_ABS, ABS_MT_TRACKING_ID, 300, 5, 0),
        ev(EV_ABS, ABS_MT_POSITION_X, 10000, 5, 0),
        ev(EV_ABS, ABS_MT_POSITION_Y, 900, 5, 0),     /* band, not core */
        ev(EV_ABS, ABS_MT_TOUCH_MAJOR, 20, 5, 0),
        ev(EV_KEY, BTN_TOUCH, 1, 5, 0),
        ev(EV_SYN, SYN_REPORT, 0, 5, 0),
    };
    feed(f, NF(f));
    CHECK(identical(f, NF(f)), "band fingertip was filtered");
}

/* ---- 4. an incomplete first frame fails open, never guesses ---- */
static void test_incomplete_fails_open(void)
{
    reset_for_test();
    struct input_event no_y[] = {
        ev(EV_ABS, ABS_MT_SLOT, 0, 6, 0),
        ev(EV_ABS, ABS_MT_TRACKING_ID, 400, 6, 0),
        ev(EV_ABS, ABS_MT_POSITION_X, 10000, 6, 0),
        ev(EV_ABS, ABS_MT_TOUCH_MAJOR, 90, 6, 0),
        ev(EV_SYN, SYN_REPORT, 0, 6, 0),
    };
    feed(no_y, NF(no_y));
    CHECK(identical(no_y, NF(no_y)), "incomplete frame was dropped");
}

/* ---- 5. a core tap is dropped without emitting anything at all ---- */
static void test_core_tap_dropped(void)
{
    reset_for_test();
    struct input_event down[] = {
        ev(EV_ABS, ABS_MT_SLOT, 0, 7, 0),
        ev(EV_ABS, ABS_MT_TRACKING_ID, 500, 7, 0),
        ev(EV_ABS, ABS_MT_POSITION_X, 10000, 7, 0),
        ev(EV_ABS, ABS_MT_POSITION_Y, 200, 7, 0),     /* core strip */
        ev(EV_ABS, ABS_MT_TOUCH_MAJOR, 20, 7, 0),     /* fingertip */
        ev(EV_KEY, BTN_TOUCH, 1, 7, 0),
        ev(EV_KEY, BTN_TOOL_FINGER, 1, 7, 0),
        ev(EV_SYN, SYN_REPORT, 0, 7, 0),
    };
    feed(down, NF(down));
    check_no_contact_payload("core DOWN");

    /* a tiny jitter below the slop must not release it */
    struct input_event jitter[] = {
        ev(EV_ABS, ABS_MT_POSITION_X, 10005, 8, 0),
        ev(EV_ABS, ABS_MT_POSITION_Y, 205, 8, 0),
        ev(EV_SYN, SYN_REPORT, 0, 8, 0),
    };
    feed(jitter, NF(jitter));
    check_no_contact_payload("core jitter");

    struct input_event up[] = {
        ev(EV_ABS, ABS_MT_TRACKING_ID, -1, 9, 0),
        ev(EV_KEY, BTN_TOUCH, 0, 9, 0),
        ev(EV_SYN, SYN_REPORT, 0, 9, 0),
    };
    feed(up, NF(up));
    check_no_contact_payload("core tap release");

    CHECK(slots[0].state == ST_EMPTY, "slot not released after tap");
}

/* ---- 6. a core swipe is released at its true origin ---- */
static void test_core_swipe_released_at_origin(void)
{
    reset_for_test();
    struct input_event down[] = {
        ev(EV_ABS, ABS_MT_SLOT, 0, 10, 0),
        ev(EV_ABS, ABS_MT_TRACKING_ID, 600, 10, 0),
        ev(EV_ABS, ABS_MT_POSITION_X, 10000, 10, 0),
        ev(EV_ABS, ABS_MT_POSITION_Y, 200, 10, 0),
        ev(EV_ABS, ABS_MT_TOUCH_MAJOR, 22, 10, 0),
        ev(EV_ABS, ABS_MT_WIDTH_MAJOR, 7, 10, 0),
        ev(EV_KEY, BTN_TOUCH, 1, 10, 0),
        ev(EV_SYN, SYN_REPORT, 0, 10, 0),
    };
    feed(down, NF(down));
    check_no_contact_payload("core swipe DOWN");

    struct input_event move[] = {
        ev(EV_ABS, ABS_MT_SLOT, 0, 11, 0),
        ev(EV_ABS, ABS_MT_POSITION_X, 10000, 11, 0),
        ev(EV_ABS, ABS_MT_POSITION_Y, 4000, 11, 0),
        ev(EV_SYN, SYN_REPORT, 0, 11, 0),
    };
    feed(move, NF(move));

    CHECK(count(EV_ABS, ABS_MT_TRACKING_ID, 600, 1) == 1, "released swipe must emit exactly one DOWN");
    CHECK(count(EV_ABS, ABS_MT_POSITION_Y, 200, 1) == 1, "DOWN must be replayed at the true origin");
    CHECK(count(EV_ABS, ABS_MT_POSITION_Y, 4000, 1) == 1, "live position must follow the origin");
    CHECK(count(EV_ABS, ABS_MT_TOUCH_MAJOR, 22, 1) == 1, "first-frame area must be replayed");
    CHECK(count(EV_KEY, BTN_TOUCH, 1, 1) == 1, "BTN_TOUCH must accompany the released contact");
    CHECK(count(EV_SYN, SYN_REPORT, 0, 0) == 1, "one frame, one SYN_REPORT");

    /* the origin must be replayed before the live position */
    int origin_at = -1, live_at = -1;
    for (int i = 0; i < out_n; i++) {
        if (out[i].type != EV_ABS || out[i].code != ABS_MT_POSITION_Y) continue;
        if (out[i].value == 200) origin_at = i;
        if (out[i].value == 4000) live_at = i;
    }
    CHECK(origin_at >= 0 && live_at > origin_at, "live position must come after the origin");
}

/* ---- 7. a held core contact is released by the hold timeout ---- */
static void test_core_hold_timeout(void)
{
    reset_for_test();
    struct input_event down[] = {
        ev(EV_ABS, ABS_MT_SLOT, 0, 12, 0),
        ev(EV_ABS, ABS_MT_TRACKING_ID, 700, 12, 0),
        ev(EV_ABS, ABS_MT_POSITION_X, 10000, 12, 0),
        ev(EV_ABS, ABS_MT_POSITION_Y, 200, 12, 0),
        ev(EV_ABS, ABS_MT_TOUCH_MAJOR, 20, 12, 0),
        ev(EV_SYN, SYN_REPORT, 0, 12, 0),
    };
    feed(down, NF(down));
    CHECK(slots[0].state == ST_HELD, "core contact should be held");

    /* pretend the hold deadline has passed */
    slots[0].held_ms -= core_hold_ms + 1;

    struct input_event tick[] = {
        ev(EV_ABS, ABS_MT_POSITION_X, 10000, 13, 0),
        ev(EV_ABS, ABS_MT_POSITION_Y, 202, 13, 0),
        ev(EV_SYN, SYN_REPORT, 0, 13, 0),
    };
    feed(tick, NF(tick));
    CHECK(count(EV_ABS, ABS_MT_TRACKING_ID, 700, 1) == 1, "expired hold must be released");
    CHECK(count(EV_ABS, ABS_MT_POSITION_Y, 200, 1) == 1, "expired hold keeps its origin");
}

/* ---- 8. disabling the core band restores plain passthrough ---- */
static void test_core_disabled(void)
{
    int saved = core_hold_ms;
    core_hold_ms = 0;
    reset_for_test();
    struct input_event f[] = {
        ev(EV_ABS, ABS_MT_SLOT, 0, 14, 0),
        ev(EV_ABS, ABS_MT_TRACKING_ID, 800, 14, 0),
        ev(EV_ABS, ABS_MT_POSITION_X, 10000, 14, 0),
        ev(EV_ABS, ABS_MT_POSITION_Y, 200, 14, 0),
        ev(EV_ABS, ABS_MT_TOUCH_MAJOR, 20, 14, 0),
        ev(EV_SYN, SYN_REPORT, 0, 14, 0),
    };
    feed(f, NF(f));
    CHECK(identical(f, NF(f)), "core band still active after being disabled");
    core_hold_ms = saved;
}

/* ---- 9. a blocked finger must not disturb a coexisting allowed finger ---- */
static void test_mixed_contacts(void)
{
    reset_for_test();
    struct input_event both[] = {
        ev(EV_ABS, ABS_MT_SLOT, 0, 15, 0),
        ev(EV_ABS, ABS_MT_TRACKING_ID, 900, 15, 0),
        ev(EV_ABS, ABS_MT_POSITION_X, 10000, 15, 0),
        ev(EV_ABS, ABS_MT_POSITION_Y, 16000, 15, 0),
        ev(EV_ABS, ABS_MT_TOUCH_MAJOR, 25, 15, 0),
        ev(EV_KEY, BTN_TOUCH, 1, 15, 0),
        ev(EV_KEY, BTN_TOOL_FINGER, 1, 15, 0),
        ev(EV_ABS, ABS_MT_SLOT, 1, 15, 0),
        ev(EV_ABS, ABS_MT_TRACKING_ID, 901, 15, 0),
        ev(EV_ABS, ABS_MT_POSITION_X, 12000, 15, 0),
        ev(EV_ABS, ABS_MT_POSITION_Y, 300, 15, 0),
        ev(EV_ABS, ABS_MT_TOUCH_MAJOR, 70, 15, 0),
        ev(EV_SYN, SYN_REPORT, 0, 15, 0),
    };
    feed(both, NF(both));
    CHECK(count(EV_ABS, ABS_MT_TRACKING_ID, 900, 1) == 1, "allowed DOWN lost");
    CHECK(count(EV_ABS, ABS_MT_TRACKING_ID, 901, 1) == 0, "blocked grip DOWN leaked");
    CHECK(count(EV_KEY, BTN_TOUCH, 1, 1) == 1, "BTN_TOUCH=1 must appear exactly once");

    struct input_event move[] = {
        ev(EV_ABS, ABS_MT_SLOT, 0, 16, 0),
        ev(EV_ABS, ABS_MT_POSITION_X, 10500, 16, 0),
        ev(EV_ABS, ABS_MT_POSITION_Y, 16500, 16, 0),
        ev(EV_SYN, SYN_REPORT, 0, 16, 0),
    };
    feed(move, NF(move));
    CHECK(count(EV_ABS, ABS_MT_POSITION_X, 10500, 1) == 1, "allowed motion lost");
    CHECK(count(EV_KEY, BTN_TOUCH, 0, 0) == 0, "BTN_TOUCH must not change during motion");
}

/* ---- 10. BTN_TOUCH must follow allowed contacts only ---- */
static void test_button_follows_allowed_contacts(void)
{
    reset_for_test();
    struct input_event both[] = {
        ev(EV_ABS, ABS_MT_SLOT, 0, 17, 0),
        ev(EV_ABS, ABS_MT_TRACKING_ID, 1000, 17, 0),
        ev(EV_ABS, ABS_MT_POSITION_X, 10000, 17, 0),
        ev(EV_ABS, ABS_MT_POSITION_Y, 16000, 17, 0),
        ev(EV_ABS, ABS_MT_TOUCH_MAJOR, 25, 17, 0),
        ev(EV_KEY, BTN_TOUCH, 1, 17, 0),
        ev(EV_ABS, ABS_MT_SLOT, 1, 17, 0),
        ev(EV_ABS, ABS_MT_TRACKING_ID, 1001, 17, 0),
        ev(EV_ABS, ABS_MT_POSITION_X, 12000, 17, 0),
        ev(EV_ABS, ABS_MT_POSITION_Y, 300, 17, 0),
        ev(EV_ABS, ABS_MT_TOUCH_MAJOR, 70, 17, 0),
        ev(EV_SYN, SYN_REPORT, 0, 17, 0),
    };
    feed(both, NF(both));
    CHECK(count(EV_KEY, BTN_TOUCH, 1, 1) == 1, "BTN_TOUCH=1 missing while a finger is allowed");

    struct input_event lift[] = {
        ev(EV_ABS, ABS_MT_SLOT, 0, 18, 0),
        ev(EV_ABS, ABS_MT_TRACKING_ID, -1, 18, 0),
        ev(EV_KEY, BTN_TOUCH, 0, 18, 0),
        ev(EV_SYN, SYN_REPORT, 0, 18, 0),
    };
    feed(lift, NF(lift));
    CHECK(count(EV_ABS, ABS_MT_TRACKING_ID, -1, 1) == 1, "allowed UP was filtered");
    CHECK(count(EV_KEY, BTN_TOUCH, 0, 1) == 1, "BTN_TOUCH=0 not forwarded");
}

/* ---- 11. every allowed contact gets exactly one DOWN and one UP ---- */
static void test_no_duplicate_tracking_ids(void)
{
    reset_for_test();
    struct input_event down[] = {
        ev(EV_ABS, ABS_MT_SLOT, 0, 19, 0),
        ev(EV_ABS, ABS_MT_TRACKING_ID, 1100, 19, 0),
        ev(EV_ABS, ABS_MT_POSITION_X, 10000, 19, 0),
        ev(EV_ABS, ABS_MT_POSITION_Y, 16000, 19, 0),
        ev(EV_ABS, ABS_MT_TOUCH_MAJOR, 25, 19, 0),
        ev(EV_SYN, SYN_REPORT, 0, 19, 0),
    };
    feed(down, NF(down));
    CHECK(count(EV_ABS, ABS_MT_TRACKING_ID, 1100, 1) == 1, "DOWN count != 1");

    struct input_event up[] = {
        ev(EV_ABS, ABS_MT_TRACKING_ID, -1, 20, 0),
        ev(EV_SYN, SYN_REPORT, 0, 20, 0),
    };
    feed(up, NF(up));
    CHECK(count(EV_ABS, ABS_MT_TRACKING_ID, -1, 1) == 1, "UP count != 1");
}

/* ---- 12. legacy axes are masked only while a contact is filtered ---- */
static void test_legacy_axes(void)
{
    reset_for_test();
    struct input_event clean[] = {
        ev(EV_ABS, ABS_MT_SLOT, 0, 21, 0),
        ev(EV_ABS, ABS_MT_TRACKING_ID, 1200, 21, 0),
        ev(EV_ABS, ABS_MT_POSITION_X, 10000, 21, 0),
        ev(EV_ABS, ABS_MT_POSITION_Y, 16000, 21, 0),
        ev(EV_ABS, ABS_MT_TOUCH_MAJOR, 25, 21, 0),
        ev(EV_ABS, ABS_X, 10000, 21, 0),
        ev(EV_ABS, ABS_Y, 16000, 21, 0),
        ev(EV_SYN, SYN_REPORT, 0, 21, 0),
    };
    feed(clean, NF(clean));
    CHECK(identical(clean, NF(clean)), "legacy axes rewritten without filtering");

    reset_for_test();
    struct input_event mixed[] = {
        ev(EV_ABS, ABS_MT_SLOT, 0, 22, 0),
        ev(EV_ABS, ABS_MT_TRACKING_ID, 1201, 22, 0),
        ev(EV_ABS, ABS_MT_POSITION_X, 12000, 22, 0),
        ev(EV_ABS, ABS_MT_POSITION_Y, 300, 22, 0),
        ev(EV_ABS, ABS_MT_TOUCH_MAJOR, 70, 22, 0),
        ev(EV_ABS, ABS_X, 12000, 22, 0),
        ev(EV_ABS, ABS_Y, 300, 22, 0),
        ev(EV_SYN, SYN_REPORT, 0, 22, 0),
    };
    feed(mixed, NF(mixed));
    CHECK(count(EV_ABS, ABS_X, 12000, 1) == 0, "blocked legacy X leaked");
    CHECK(count(EV_ABS, ABS_Y, 300, 1) == 0, "blocked legacy Y leaked");
}

/* ---- 13. a source event overflow must stop the filter, not guess ---- */
static void test_syn_dropped_is_fatal(void)
{
    reset_for_test();
    struct input_event drop = ev(EV_SYN, SYN_DROPPED, 0, 23, 0);
    CHECK(handle_event(&drop) != 0, "SYN_DROPPED must abort the filter");
    frame_count = 0;
}

/* ---- 14. a frame larger than MAX_FRAME must abort, not truncate ---- */
static void test_frame_overflow_is_fatal(void)
{
    reset_for_test();
    struct input_event e = ev(EV_ABS, ABS_MT_SLOT, 0, 24, 0);
    int rejected = 0;
    for (int i = 0; i < MAX_FRAME + 1; i++)
        if (handle_event(&e) != 0) rejected = 1;
    CHECK(rejected, "frame overflow was silently accepted");
    frame_count = 0;
}

/* ---- 15. rotation-aware band mapping ---- */
static void test_rotation_mapping(void)
{
    rot = 0;
    CHECK(in_band(500, 16000), "portrait left band missed");
    CHECK(in_band(21000, 16000), "portrait right band missed");
    CHECK(!in_band(10000, 16000), "portrait centre wrongly in band");

    rot = 1;
    CHECK(in_band(10000, 500), "landscape left band missed");
    CHECK(in_band(10000, 31000), "landscape right band missed");
    CHECK(!in_band(10000, 16000), "landscape centre wrongly in band");
    CHECK(in_core(10000, 200), "landscape core strip missed");
    CHECK(!in_core(10000, 900), "landscape core strip too wide");

    band_l = 0; band_r = 0;
    CHECK(!in_band(10000, 10), "disabled band still filtered");
    band_l = band_r = 150;
}

/*
 * ---- 16. randomised sequences ----
 * Rebuilds the virtual device state purely from the emitted stream and asserts
 * it equals "every contact the filter decided to forward".
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
    int next_tid;
    int source_btn;

    for (unsigned seed = 1; seed <= 40; seed++) {
        rng_state = seed * 7919u;
        reset_for_test();
        memset(gen_down, 0, sizeof(gen_down));
        next_tid = 1000;
        source_btn = 0;

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
                    unsigned pick = rnd() % 3;
                    if (pick == 0) y = (int)(rnd() % 700);              /* core + band */
                    else if (pick == 1) y = (int)(rnd() % 1600);        /* band only */
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

            CHECK(out_n > 0 && out[out_n - 1].type == EV_SYN &&
                  out[out_n - 1].code == SYN_REPORT, "seed %u step %d: bad terminator", seed, step);

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
                int expected = slots[i].state == ST_PASS;
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
    test_band_fingertip_passes();
    test_incomplete_fails_open();
    test_core_tap_dropped();
    test_core_swipe_released_at_origin();
    test_core_hold_timeout();
    test_core_disabled();
    test_mixed_contacts();
    test_button_follows_allowed_contacts();
    test_no_duplicate_tracking_ids();
    test_legacy_axes();
    test_syn_dropped_is_fatal();
    test_frame_overflow_is_fatal();
    test_rotation_mapping();
    test_randomised_sequences();

    fprintf(stderr, "\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
