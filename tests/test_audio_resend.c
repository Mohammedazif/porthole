/* One lost AirPlay audio packet must not stall all later audio for the whole
 * 256-packet ring (~2.8 s of AAC-ELD). Drives Porthole's raop_buffer.c with
 * encrypted-looking RTP packets; the payload decrypts to noise, which is fine
 * because only sequencing is under test. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logger.h"
#include "raop_buffer.h"

static int failures;
#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                         \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static void enqueue(raop_buffer_t *buffer, unsigned short seqnum)
{
    unsigned char packet[12 + 32] = {0x80, 0x60};
    packet[2] = (unsigned char)(seqnum >> 8);
    packet[3] = (unsigned char)seqnum;
    packet[7] = (unsigned char)seqnum; /* RTP timestamp, unused here */
    memset(packet + 12, 0x5a, 32);
    CHECK(raop_buffer_enqueue(buffer, packet, sizeof packet, 1) == 1);
}

/* Returns the dequeued seqnum, or -1 when the buffer yields nothing. */
static int dequeue(raop_buffer_t *buffer)
{
    unsigned int length = 0;
    uint32_t timestamp = 0;
    unsigned short seqnum = 0;
    void *payload = raop_buffer_dequeue(buffer, &length, &timestamp, &seqnum, 0);
    if (!payload)
        return -1;
    free(payload);
    return seqnum;
}

int main(void)
{
    const unsigned char key[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    const unsigned char iv[16] = {0};
    logger_t *logger = logger_init();
    raop_buffer_t *buffer = raop_buffer_init(logger, key, iv);
    CHECK(logger && buffer);
    if (!buffer)
        return 1;

    enqueue(buffer, 100);
    CHECK(dequeue(buffer) == 100);

    /* 101 is lost. A resend may still arrive shortly: wait while few later
     * packets are buffered. */
    int waited = 0;
    unsigned short next = 102;
    for (; next < 102 + 22; next++) {
        enqueue(buffer, next);
        if (dequeue(buffer) == -1)
            waited++;
    }
    CHECK(waited == 22);

    /* A late resend inside that window is still used, in order. */
    enqueue(buffer, 101);
    CHECK(dequeue(buffer) == 101);
    CHECK(dequeue(buffer) == 102);
    for (int seq = 103; seq < 102 + 22; seq++)
        CHECK(dequeue(buffer) == seq);
    CHECK(dequeue(buffer) == -1);

    /* Next loss (124) never resent: give up after about 24 packets, not 256. */
    int stalled = 0;
    int resumed_at = -1;
    for (next = 125; next < 125 + 200 && resumed_at < 0; next++) {
        enqueue(buffer, next);
        int got = dequeue(buffer);
        if (got == -1)
            got = dequeue(buffer); /* the skip itself returns nothing */
        if (got == -1)
            stalled++;
        else
            resumed_at = got;
    }
    CHECK(resumed_at == 125);
    CHECK(stalled >= 20 && stalled <= 24);
    printf("lost packet awaited for %d later packets, then skipped\n", stalled);

    raop_buffer_destroy(buffer);
    logger_destroy(logger);
    if (failures)
        return 1;
    puts("bounded audio resend wait passed");
    return 0;
}
