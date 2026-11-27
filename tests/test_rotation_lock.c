#include <stdio.h>

#include "control/rotation_lock.h"

static int failures;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition);         \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

/* Lock on, from a real log: YouTube full screen tapped upright (screen 8),
 * then held top right (4), then top left (3); the screen stayed 8. */
static void test_lock_on_youtube(void)
{
    ph_rotation_lock r;
    ph_rotation_lock_begin(&r, PH_ROTATION_LOCK_UNKNOWN);
    CHECK(!ph_rotation_lock_observe(&r, 1, 1, 54000));
    CHECK(!ph_rotation_lock_observe(&r, 8, 1, 334000));
    CHECK(r.lock == PH_ROTATION_LOCK_UNKNOWN);
    CHECK(ph_rotation_lock_pointer_turn(&r, 8, 1) == 1); /* upright: same either way */
    CHECK(ph_rotation_lock_observe(&r, 8, 4, 345000));
    CHECK(r.lock == 1);
    CHECK(ph_rotation_lock_pointer_turn(&r, 8, 4) == 1); /* still upright, no jump */
    CHECK(!ph_rotation_lock_observe(&r, 8, 3, 354000));
    CHECK(r.lock == 1 && ph_rotation_lock_pointer_turn(&r, 8, 3) == 1);
    /* Measured the day before: screen 6, held either way, wanted upright. */
    CHECK(ph_rotation_lock_pointer_turn(&r, 6, 4) == 1 && ph_rotation_lock_pointer_turn(&r, 6, 3) == 1);
    /* A portrait screen is as shown, whatever the lock. */
    CHECK(ph_rotation_lock_pointer_turn(&r, 1, 4) == 0);
}

/* Lock off, from the same log: the screen turns with the phone, the sensor
 * a little before (392) or after (398) it. */
static void test_lock_off_follows(void)
{
    ph_rotation_lock r;
    ph_rotation_lock_begin(&r, 1); /* remembered on from before */
    CHECK(!ph_rotation_lock_observe(&r, 1, 1, 388000));
    CHECK(!ph_rotation_lock_observe(&r, 1, 3, 392269));
    CHECK(ph_rotation_lock_observe(&r, 8, 3, 392745));
    CHECK(r.lock == 0);
    CHECK(ph_rotation_lock_pointer_turn(&r, 8, 3) == 0);
    /* Flipped: the screen first, the sensor 0.6 s later. In between the
     * pointer follows the sensor; the lock stays off. */
    CHECK(!ph_rotation_lock_observe(&r, 6, 3, 398626));
    CHECK(ph_rotation_lock_pointer_turn(&r, 6, 3) == 8);
    CHECK(!ph_rotation_lock_observe(&r, 6, 4, 399268));
    CHECK(r.lock == 0 && ph_rotation_lock_pointer_turn(&r, 6, 4) == 0);
    /* An app landscape with the phone upright: a quarter turn. */
    CHECK(ph_rotation_lock_pointer_turn(&r, 8, 1) == 1);
}

static void test_no_guess_from_the_start(void)
{
    ph_rotation_lock r;
    ph_rotation_lock_begin(&r, PH_ROTATION_LOCK_UNKNOWN);
    /* Sharing begins already in landscape, in step: nothing is learned. */
    CHECK(!ph_rotation_lock_observe(&r, 6, 4, 1000));
    CHECK(!ph_rotation_lock_observe(&r, 6, 4, 2200));
    CHECK(r.lock == PH_ROTATION_LOCK_UNKNOWN && ph_rotation_lock_pointer_turn(&r, 6, 4) == 0);
    /* Turned in step long after the screen went landscape: no verdict. */
    ph_rotation_lock_begin(&r, PH_ROTATION_LOCK_UNKNOWN);
    ph_rotation_lock_observe(&r, 1, 1, 1000);
    ph_rotation_lock_observe(&r, 8, 1, 5000);
    CHECK(!ph_rotation_lock_observe(&r, 8, 3, 20000));
    CHECK(r.lock == PH_ROTATION_LOCK_UNKNOWN);
    /* A portrait screen while held sideways says nothing either. */
    ph_rotation_lock_begin(&r, PH_ROTATION_LOCK_UNKNOWN);
    ph_rotation_lock_observe(&r, 1, 1, 1000);
    CHECK(!ph_rotation_lock_observe(&r, 1, 4, 9000));
    CHECK(r.lock == PH_ROTATION_LOCK_UNKNOWN);
    ph_rotation_lock_begin(&r, 7);
    CHECK(r.lock == PH_ROTATION_LOCK_UNKNOWN);
}

int main(void)
{
    test_lock_on_youtube();
    test_lock_off_follows();
    test_no_guess_from_the_start();
    if (failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
