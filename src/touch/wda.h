#ifndef PORTHOLE_TOUCH_WDA_H
#define PORTHOLE_TOUCH_WDA_H

#include <stddef.h>
#include <stdint.h>

typedef struct ph_wda ph_wda;

ph_wda *ph_wda_create(const char *host, int port);
void ph_wda_destroy(ph_wda *wda);
void ph_wda_set_send(ph_wda *wda, int (*send)(void *user, const uint8_t *data, size_t len),
                     void *user);

/* POST /session {"capabilities":{"alwaysMatch":{"platformName":"iOS"}}}. */
int ph_wda_create_session(ph_wda *wda);

/* Reads "sessionId":"..." from a WebDriverAgent response body. */
int ph_wda_note_response(ph_wda *wda, const char *http_body);

int ph_wda_tap(ph_wda *wda, double x, double y);
int ph_wda_drag(ph_wda *wda, double x1, double y1, double x2, double y2, double seconds);

/* scale 1 sends nothing and returns 0. scale <= 0 returns -1. */
int ph_wda_pinch(ph_wda *wda, double cx, double cy, double scale);

#endif
