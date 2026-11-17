/* The real gesture and pointer state machines against a paced physical peer.
 * Reports apply later than enqueue; ACKs lag that application; video follows ACK.
 * This exercises actual button state, coordinates and cancellation, not counters
 * mirroring the gesture implementation. No SDL, radio or platform APIs needed. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "control/gesture.h"

static int failures;
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); failures++; \
} } while (0)

typedef struct wire {
    ph_hid_mouse mouse;
    uint32_t fence;
    uint64_t due;
    bool ack, done;
} wire;

typedef struct physical_event {
    ph_hid_mouse mouse;
    uint64_t at;
    double x, y;
} physical_event;

typedef struct fixture {
    ph_pointer pointer;
    ph_gesture gesture;
    wire queued[2048];
    physical_event applied[2048];
    size_t queued_count, applied_count;
    uint64_t now, last_report, press_ack[4], release_ack[4];
    unsigned int presses_acked, releases_acked, anchors;
    unsigned int report_interval, ack_lag;
    int width, height;
    double x, y;
    uint8_t buttons;
    bool freeze_video, freeze_on_held_motion, lose_acks;
} fixture;

static void mouse(void *user, const ph_hid_mouse *report)
{
    fixture *f = user;
    CHECK(f->queued_count < sizeof f->queued / sizeof f->queued[0]);
    if (f->queued_count >= sizeof f->queued / sizeof f->queued[0])
        return;
    uint64_t due = f->last_report > f->now ? f->last_report : f->now;
    due += f->report_interval;
    f->last_report = due;
    f->queued[f->queued_count++] = (wire){.mouse = *report, .due = due};
}

static void delay(void *user, uint16_t ms)
{
    fixture *f = user;
    if (f->last_report < f->now)
        f->last_report = f->now;
    f->last_report += ms;
}

static void sync_reports(void *user, uint32_t id)
{
    fixture *f = user;
    CHECK(f->queued_count < sizeof f->queued / sizeof f->queued[0]);
    if (f->queued_count >= sizeof f->queued / sizeof f->queued[0])
        return;
    f->queued[f->queued_count++] = (wire){.fence = id, .due = f->last_report + f->ack_lag,
                                         .ack = true};
}

static void init(fixture *f, int width, int height)
{
    memset(f, 0, sizeof *f);
    f->now = 100;
    f->width = width;
    f->height = height;
    f->report_interval = 15;
    f->ack_lag = 120;
    f->pointer.output = (ph_pointer_output){f, mouse, delay, sync_reports};
    ph_pointer_set_enabled(&f->pointer, true, f->now);
    double aspect = (double)width / height;
    double model_width = 500 * fmax(1, aspect);
    f->x = model_width / 2;
    f->y = model_width / aspect / 2;
    f->pointer.aspect = aspect;
    f->pointer.actual_x = f->pointer.actual_y = .5;
    f->pointer.known = true;
    f->pointer.observed_at = f->now;
    ph_gesture_init(&f->gesture, &f->pointer);
}

static double travel(int value)
{
    double magnitude = fabs((double)value);
    double pixels = .25 * magnitude + .055 * magnitude * magnitude;
    return value < 0 ? -pixels : pixels;
}

static void step(fixture *f)
{
    f->now += 10;
    for (size_t i = 0; i < f->queued_count; i++) {
        wire *w = &f->queued[i];
        if (w->done || w->due > f->now)
            continue;
        w->done = true;
        if (w->ack) {
            if (f->lose_acks)
                continue;
            if (w->fence == f->pointer.sync_id && f->pointer.phase == PH_POINTER_BUTTON) {
                if (f->pointer.buttons && f->presses_acked < 4)
                    f->press_ack[f->presses_acked++] = f->now;
                else if (!f->pointer.buttons && f->releases_acked < 4)
                    f->release_ack[f->releases_acked++] = f->now;
            }
            ph_pointer_ack(&f->pointer, w->fence, f->now);
            continue;
        }
        ph_hid_mouse m = w->mouse;
        double aspect = f->pointer.aspect;
        double width = 500 * fmax(1, aspect), height = width / aspect;
        f->x = fmin(width - 16, fmax(16, f->x + travel(m.dx)));
        f->y = fmin(height - 16, fmax(16, f->y + travel(m.dy)));
        f->buttons = m.buttons;
        if (m.dx == -127 && m.dy == -127) {
            f->anchors++;
            CHECK(!m.buttons);
        }
        if (m.buttons && (m.dx || m.dy) && f->freeze_on_held_motion)
            f->freeze_video = true;
        CHECK(f->applied_count < sizeof f->applied / sizeof f->applied[0]);
        if (f->applied_count < sizeof f->applied / sizeof f->applied[0])
            f->applied[f->applied_count++] = (physical_event){m, f->now, f->x, f->y};
    }
    if (!f->freeze_video && f->pointer.phase == PH_POINTER_WAIT_VIDEO
        && f->now >= f->pointer.wait_since && f->now - f->pointer.wait_since >= 40) {
        double aspect = f->pointer.aspect, width = 500 * fmax(1, aspect);
        ph_pointer_observe(&f->pointer, true, f->x / width, f->y * aspect / width, f->now);
    }
    if (ph_gesture_active(&f->gesture))
        ph_gesture_tick(&f->gesture, f->width, f->height, f->now);
    else
        ph_pointer_tick(&f->pointer, f->now);
}

static void settle(fixture *f)
{
    for (int i = 0; i < 3500; i++) {
        bool pending = false;
        for (size_t j = 0; j < f->queued_count; j++)
            pending |= !f->queued[j].done;
        if (!ph_gesture_active(&f->gesture) && !pending
            && (f->pointer.phase == PH_POINTER_IDLE || ph_pointer_transport_stalled(&f->pointer)))
            return;
        step(f);
    }
    CHECK(false);
}

static ph_gesture_request request(const fixture *f, ph_gesture_kind kind)
{
    return (ph_gesture_request){.kind = kind, .start_x = .5, .start_y = .5,
        .end_x = .5, .end_y = .2, .width = f->width, .height = f->height};
}

static size_t press_count(const fixture *f)
{
    size_t count = 0;
    uint8_t previous = 0;
    for (size_t i = 0; i < f->applied_count; i++) {
        if (f->applied[i].mouse.buttons && !previous)
            count++;
        previous = f->applied[i].mouse.buttons;
    }
    return count;
}

static uint64_t release_time(const fixture *f, size_t which)
{
    size_t count = 0;
    uint8_t previous = 0;
    for (size_t i = 0; i < f->applied_count; i++) {
        if (!f->applied[i].mouse.buttons && previous && count++ == which)
            return f->applied[i].at;
        previous = f->applied[i].mouse.buttons;
    }
    return 0;
}

static void wait_pressed(fixture *f)
{
    for (int i = 0; i < 1000 && !f->presses_acked; i++)
        step(f);
    CHECK(f->presses_acked == 1 && f->buttons == PH_HID_BTN_LEFT);
}

static void taps_and_holds(void)
{
    fixture f;
    for (int kind = PH_GESTURE_TAP; kind <= PH_GESTURE_HOLD; kind++) {
        init(&f, 500, 1080);
        f.now += UINT64_C(1) << 33;
        f.pointer.observed_at = f.now;
        ph_gesture_request r = request(&f, (ph_gesture_kind)kind);
        r.press_ms = 110;
        r.gap_ms = 130;
        r.duration_ms = 430;
        CHECK(ph_gesture_start(&f.gesture, &r, f.now));
        CHECK(!ph_gesture_start(&f.gesture, &r, f.now));
        settle(&f);
        size_t expected = kind == PH_GESTURE_DOUBLE_TAP ? 2 : 1;
        CHECK(ph_gesture_get_result(&f.gesture) == PH_GESTURE_SUCCEEDED);
        CHECK(press_count(&f) == expected && f.presses_acked == expected);
        CHECK(!f.buttons && !f.pointer.buttons && !f.pointer.count);
        CHECK(release_time(&f, 0) >= f.press_ack[0] + (kind == PH_GESTURE_HOLD ? 430 : 110));
        if (expected == 2) {
            size_t presses = 0;
            uint8_t previous = 0;
            for (size_t i = 0; i < f.applied_count; i++) {
                if (f.applied[i].mouse.buttons && !previous && ++presses == 2)
                    CHECK(f.applied[i].at >= f.release_ack[0] + 130);
                previous = f.applied[i].mouse.buttons;
            }
            CHECK(release_time(&f, 1) >= f.press_ack[1] + 110);
        }
        CHECK(!f.anchors);
    }
}

static void four_way_swipes(void)
{
    const double ends[][2] = {{.5,.2},{.5,.8},{.2,.5},{.8,.5}};
    const int shapes[][2] = {{500,1080},{1080,500}};
    fixture f;
    for (size_t shape = 0; shape < 2; shape++) {
        for (size_t direction = 0; direction < 4; direction++) {
            init(&f, shapes[shape][0], shapes[shape][1]);
            ph_gesture_request r = request(&f, PH_GESTURE_SWIPE);
            r.end_x = ends[direction][0]; r.end_y = ends[direction][1];
            r.duration_ms = 350;
            CHECK(ph_gesture_start(&f.gesture, &r, f.now));
            settle(&f);
            CHECK(ph_gesture_get_result(&f.gesture) == PH_GESTURE_SUCCEEDED);
            CHECK(press_count(&f) == 1 && !f.buttons && !f.pointer.buttons);
            double aspect = (double)r.width/r.height, width = 500*fmax(1,aspect);
            CHECK(hypot(f.x-r.end_x*width,f.y-r.end_y*width/aspect) <= 4);
            bool moved_held = false;
            for (size_t i = 0; i < f.applied_count; i++) {
                if (f.applied[i].mouse.dx || f.applied[i].mouse.dy) {
                    CHECK(f.applied[i].mouse.buttons == PH_HID_BTN_LEFT);
                    CHECK(f.applied[i].at >= f.press_ack[0]);
                    moved_held = true;
                }
            }
            CHECK(moved_held && !f.anchors);
            CHECK(release_time(&f,0) >= f.press_ack[0] + 350);
        }
    }
}

static void edge_coordinates(void)
{
    const double ends[][2] = {{0,.5},{.999,.5},{.5,0},{.5,.999}};
    const int shapes[][2] = {{500,1080},{1080,500}};
    fixture f;
    for (size_t shape=0;shape<2;shape++) {
        for (size_t direction=0;direction<4;direction++) {
            for (int swipe=0;swipe<2;swipe++) {
                init(&f,shapes[shape][0],shapes[shape][1]);
                ph_gesture_request r=request(&f,swipe ? PH_GESTURE_SWIPE : PH_GESTURE_TAP);
                r.end_x=ends[direction][0];r.end_y=ends[direction][1];
                if (!swipe) {r.start_x=r.end_x;r.start_y=r.end_y;}
                CHECK(ph_gesture_start(&f.gesture,&r,f.now));
                settle(&f);
                CHECK(ph_gesture_get_result(&f.gesture)==PH_GESTURE_SUCCEEDED);
                double aspect=(double)r.width/r.height,width=500*fmax(1,aspect),height=width/aspect;
                double x=fmin(width-16,fmax(16,r.end_x*width));
                double y=fmin(height-16,fmax(16,r.end_y*height));
                CHECK(hypot(f.x-x,f.y-y)<=4 && !f.buttons && press_count(&f)==1);
            }
        }
    }
}

static void double_tap_timing_policy(void)
{
    fixture f;
    init(&f,500,1080);
    f.ack_lag=600;
    ph_gesture_request r=request(&f,PH_GESTURE_DOUBLE_TAP);
    CHECK(ph_gesture_start(&f.gesture,&r,f.now));
    settle(&f);
    CHECK(ph_gesture_get_result(&f.gesture)==PH_GESTURE_FAILED);
    CHECK(press_count(&f)==1 && !f.buttons && !f.pointer.buttons);
    CHECK(f.releases_acked==1 && !ph_pointer_transport_stalled(&f.pointer));
    /* A caller can choose a wider explicit policy, but not an unbounded one. */
    init(&f,500,1080);
    r=request(&f,PH_GESTURE_DOUBLE_TAP);
    r.gap_ms=1000;
    CHECK(!ph_gesture_start(&f.gesture,&r,f.now));
    r.double_tap_window_ms=1001;
    CHECK(!ph_gesture_start(&f.gesture,&r,f.now));
    r.double_tap_window_ms=1000;
    CHECK(!ph_gesture_start(&f.gesture,&r,f.now));
    r.gap_ms=80;
    CHECK(ph_gesture_start(&f.gesture,&r,f.now));
    settle(&f);
    CHECK(ph_gesture_get_result(&f.gesture)==PH_GESTURE_SUCCEEDED && press_count(&f)==2);
}

static void failures_and_cancellation(void)
{
    fixture f;
    for (int reason = 0; reason < 5; reason++) {
        init(&f,500,1080);
        ph_gesture_request r = request(&f,PH_GESTURE_SWIPE);
        if (reason == 0) f.lose_acks = true;
        if (reason == 1) f.freeze_on_held_motion = true;
        if (reason == 4) { r.kind=PH_GESTURE_HOLD;r.duration_ms=5000;r.timeout_ms=350; }
        CHECK(ph_gesture_start(&f.gesture,&r,f.now));
        if (reason == 2 || reason == 3) {
            wait_pressed(&f);
            if (reason == 2)
                ph_gesture_tick(&f.gesture,1080,500,++f.now);
            else {
                ph_gesture_cancel(&f.gesture,++f.now); /* focus/escape caller path */
                size_t queued=f.queued_count;
                ph_gesture_cancel(&f.gesture,++f.now);
                CHECK(f.queued_count==queued);
            }
        }
        settle(&f);
        CHECK(!ph_gesture_active(&f.gesture) && !f.buttons && !f.pointer.buttons);
        CHECK(!f.pointer.count && !f.pointer.requested_buttons && press_count(&f)==1);
        CHECK(ph_gesture_get_result(&f.gesture) ==
              (reason==4 ? PH_GESTURE_TIMED_OUT : reason>=2 ? PH_GESTURE_CANCELLED : PH_GESTURE_FAILED));
        CHECK(ph_pointer_transport_stalled(&f.pointer) == (reason==0));
        size_t applied=f.applied_count;
        for (int i=0;i<500;i++) step(&f);
        CHECK(f.applied_count==applied); /* no automatic replay */
    }
    /* Initial frozen video never permits an estimated press or recovery sweep. */
    init(&f,500,1080);
    f.freeze_video=true;
    ph_gesture_request r=request(&f,PH_GESTURE_TAP);
    r.start_x=.8;
    CHECK(ph_gesture_start(&f.gesture,&r,f.now));
    settle(&f);
    CHECK(ph_gesture_get_result(&f.gesture)==PH_GESTURE_FAILED && !press_count(&f));
    CHECK(!f.buttons && !ph_pointer_transport_stalled(&f.pointer));
}

static void hover_and_rejection(void)
{
    fixture f;
    init(&f,500,1080);
    CHECK(ph_pointer_target(&f.pointer,.8,.8,500.0/1080,f.now));
    uint32_t old=f.pointer.sync_id;
    ph_gesture_request r=request(&f,PH_GESTURE_TAP);
    CHECK(ph_gesture_start(&f.gesture,&r,++f.now));
    CHECK(f.pointer.sync_id==old && f.pointer.phase==PH_POINTER_DRAINING);
    settle(&f);
    CHECK(ph_gesture_get_result(&f.gesture)==PH_GESTURE_SUCCEEDED && press_count(&f)==1);
    CHECK(!f.anchors && hypot(f.x-250,f.y-540)<=4);

    init(&f,500,1080);
    r=request(&f,PH_GESTURE_TAP);
    ph_gesture_request bad=r;
    bad.start_x=NAN;CHECK(!ph_gesture_start(&f.gesture,&bad,f.now));
    bad=r;bad.width=0;CHECK(!ph_gesture_start(&f.gesture,&bad,f.now));
    bad=r;bad.press_ms=79;CHECK(!ph_gesture_start(&f.gesture,&bad,f.now));
    bad=r;bad.end_y=1;CHECK(!ph_gesture_start(&f.gesture,&bad,f.now));
    f.pointer.buttons=PH_HID_BTN_LEFT;CHECK(!ph_gesture_start(&f.gesture,&r,f.now));
    f.pointer.buttons=0;f.pointer.requested_buttons=PH_HID_BTN_RIGHT;
    CHECK(!ph_gesture_start(&f.gesture,&r,f.now));
    f.pointer.requested_buttons=0;f.pointer.count=1;
    CHECK(!ph_gesture_start(&f.gesture,&r,f.now));
    f.pointer.count=0;f.pointer.enabled=false;
    CHECK(!ph_gesture_start(&f.gesture,&r,f.now));
    f.pointer.enabled=true;f.pointer.output.sync=NULL;
    CHECK(!ph_gesture_start(&f.gesture,&r,f.now));
    CHECK(!f.queued_count && !ph_gesture_active(&f.gesture));
    ph_gesture_cancel(&f.gesture,f.now);
    CHECK(!f.queued_count); /* inactive cancel cannot erase unrelated pointer work */
}

int main(void)
{
    taps_and_holds();
    four_way_swipes();
    edge_coordinates();
    double_tap_timing_policy();
    failures_and_cancellation();
    hover_and_rejection();
    if (failures) return 1;
    puts("all paced gesture, ACK-clock, held-state and cancellation checks passed");
    return 0;
}
