/* Audio/video PTS mapping: in sync, early, late, zero, backward jump, hold cap. */
#include <stdio.h>
#include <stdint.h>

#include "audio/sync.h"

static int g_failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);               \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

static void expect(ph_sync *sync, uint64_t pts, ph_sync_kind kind, int ms)
{
    ph_sync_action action = ph_sync_audio(sync, pts, 0);
    CHECK(action.kind == kind);
    CHECK(action.hold_or_drop_ms == ms);
}

int main(void)
{
    const uint64_t ns = 1000000ULL;
    const uint64_t t = 5000ULL * 1000ULL * ns;
    ph_sync *sync = ph_sync_create();
    ph_sync_action action;

    CHECK(sync);
    if (!sync)
        return 1;

    expect(sync, t, PH_SYNC_PLAY, 0);

    /* Audio ahead of the picture waits for it; audio level with or behind the
     * picture plays at once, because delaying late audio only makes it later. */
    ph_sync_note_video(sync, t);
    expect(sync, t, PH_SYNC_PLAY, 0);
    expect(sync, t + 40 * ns, PH_SYNC_PLAY, 0);
    expect(sync, t - 20 * ns, PH_SYNC_PLAY, 0);
    expect(sync, t + 41 * ns, PH_SYNC_HOLD, 41);
    expect(sync, t - 21 * ns, PH_SYNC_PLAY, 0);
    expect(sync, t - 400 * ns, PH_SYNC_PLAY, 0);
    expect(sync, t + 400 * ns, PH_SYNC_HOLD, 200);
    action = ph_sync_audio(sync, t, 999);
    CHECK(action.kind == PH_SYNC_PLAY && action.hold_or_drop_ms == 0);
    CHECK(action.have_lead && action.lead_ms == 0);

    action = ph_sync_audio(sync, 0, 50);
    CHECK(action.kind == PH_SYNC_PLAY && action.hold_or_drop_ms == 0);
    expect(sync, t + 100 * ns, PH_SYNC_PLAY, 0);
    ph_sync_note_video(sync, 0);
    expect(sync, t + 100 * ns, PH_SYNC_PLAY, 0);

    /* A lead beyond one second is a broken mapping, not something to wait for. */
    ph_sync_reset(sync);
    ph_sync_note_video(sync, t);
    expect(sync, t + 2500 * ns, PH_SYNC_PLAY, 0);

    ph_sync_reset(sync);
    ph_sync_note_video(sync, t);
    expect(sync, t, PH_SYNC_PLAY, 0);
    expect(sync, t - 500 * ns, PH_SYNC_PLAY, 0);
    expect(sync, t - 1001 * ns, PH_SYNC_PLAY, 0);

    ph_sync_reset(sync);
    ph_sync_note_video(sync, t);
    expect(sync, t, PH_SYNC_PLAY, 0);
    ph_sync_note_video(sync, t - 700 * ns);
    expect(sync, t - 700 * ns, PH_SYNC_PLAY, 0);
    expect(sync, t - 620 * ns, PH_SYNC_HOLD, 80);
    ph_sync_note_video(sync, 0);
    expect(sync, t, PH_SYNC_PLAY, 0);

    /* AirPlay sends no frames for a still screen. The picture timeline still
     * advances in real time, so a stale video PTS must not look like a lead. */
    ph_sync_reset(sync);
    ph_sync_note_video_at(sync, t, 1000);
    action = ph_sync_audio_at(sync, t + 500 * ns, 0, 1500);
    CHECK(action.kind == PH_SYNC_PLAY && action.lead_ms == 0);
    action = ph_sync_audio_at(sync, t + 600 * ns, 0, 1500);
    CHECK(action.kind == PH_SYNC_HOLD && action.hold_or_drop_ms == 100);
    action = ph_sync_audio_at(sync, t + 610 * ns, 0, 900); /* clock before video: no elapsed */
    CHECK(action.kind == PH_SYNC_HOLD && action.hold_or_drop_ms == 200);

    /* Positive offset delays audio; negative cannot make late audio earlier. */
    ph_sync_reset(sync);
    ph_sync_set_offset_ms(sync, 100);
    ph_sync_note_video(sync, t);
    expect(sync, t, PH_SYNC_HOLD, 100);
    ph_sync_set_offset_ms(sync, -100);
    expect(sync, t, PH_SYNC_PLAY, 0);
    ph_sync_set_offset_ms(sync, 900);
    expect(sync, t, PH_SYNC_HOLD, 200);
    ph_sync_set_offset_ms(sync, -900);
    expect(sync, t, PH_SYNC_PLAY, 0);
    ph_sync_reset(sync);
    ph_sync_note_video(sync, t);
    expect(sync, t + 540 * ns, PH_SYNC_PLAY, 0); /* -500 kept across reset */
    expect(sync, t + 541 * ns, PH_SYNC_HOLD, 41);
    ph_sync_set_offset_ms(sync, 0);
    expect(sync, t + 541 * ns, PH_SYNC_HOLD, 200);

    ph_sync_note_video(NULL, t);
    ph_sync_reset(NULL);
    ph_sync_set_offset_ms(NULL, 10);
    action = ph_sync_audio(NULL, t, 0);
    CHECK(action.kind == PH_SYNC_PLAY && action.hold_or_drop_ms == 0);
    ph_sync_destroy(sync);
    ph_sync_destroy(NULL);

    if (g_failures) {
        fprintf(stderr, "%d sync check(s) failed\n", g_failures);
        return 1;
    }
    printf("sync in-range, ahead, late, still-screen clock, zero, backward jump and hold cap passed\n");
    return 0;
}
