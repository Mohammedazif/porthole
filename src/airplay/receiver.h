#ifndef PORTHOLE_AIRPLAY_RECEIVER_H
#define PORTHOLE_AIRPLAY_RECEIVER_H

/* Wi-Fi screen receiver: makes this PC show up in the iPhone's Screen Mirroring list and
 * delivers the mirrored screen as an H.264 byte stream. Built on UxPlay's AirPlay core. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct ph_receiver ph_receiver;

typedef struct ph_receiver_callbacks {
    void *user;
    /* One H.264 access unit in Annex-B format (start codes; SPS/PPS precede keyframes).
     * Called on a network thread; `data` is only valid during the call. */
    void (*video)(void *user, const uint8_t *data, size_t len, uint64_t timestamp_ns);
    /* Decrypted compressed audio and its negotiated format, on network threads.
     * timestamp_ns is the iPhone's clock. due_in_us is how long from now until
     * the iPhone expects it heard, or INT64_MIN before the clocks are synced. */
    void (*audio)(void *user, unsigned char compression, const uint8_t *data, size_t len,
                  uint64_t timestamp_ns, int64_t due_in_us);
    void (*audio_format)(void *user, unsigned char compression, unsigned short samples);
    void (*audio_volume)(void *user, float decibels);
    void (*audio_flush)(void *user);
    /* A device started a mirroring session. Called on a network thread. */
    void (*connected)(void *user, const char *name, const char *model);
    /* The mirroring session ended or was reset. Called on a network thread. */
    void (*disconnected)(void *user);
    /* Return false to refuse this device. NULL admits every device. */
    bool (*admit)(void *user, const char *device_id, const char *name, const char *model);
} ph_receiver_callbacks;

typedef struct ph_receiver_options {
    const char *name;     /* shown in the iPhone's Screen Mirroring list */
    unsigned short width; /* display size advertised to the iPhone (it scales to fit) */
    unsigned short height;
    unsigned short max_fps;
    bool fixed_ports; /* TCP 7000/7001/7100 and UDP 6000/6001/7011, for firewall rules */
    bool debug;       /* verbose protocol logging */
} ph_receiver_options;

/* Starts advertising and accepting connections. Returns NULL on failure (logged). */
ph_receiver *ph_receiver_start(const ph_receiver_options *opt, const ph_receiver_callbacks *cb);

/* Call regularly from the main thread: restarts the server after a dropped connection. */
void ph_receiver_disconnect(ph_receiver *r);
void ph_receiver_poll(ph_receiver *r);

void ph_receiver_stop(ph_receiver *r);

#endif
