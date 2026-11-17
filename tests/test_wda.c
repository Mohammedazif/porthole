/* WebDriverAgent HTTP client. The send callback captures bytes; nothing is dialed. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "touch/wda.h"

static int g_failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);               \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

typedef struct capture {
    char data[8192];
    size_t len;
    int calls;
    int fail;
} capture;

static int capture_send(void *user, const uint8_t *data, size_t len)
{
    capture *cap = user;

    cap->calls++;
    if (cap->fail || len >= sizeof cap->data)
        return -1;
    memcpy(cap->data, data, len);
    cap->data[len] = '\0';
    cap->len = len;
    return 0;
}

static int count_text(const char *haystack, const char *needle)
{
    int n = 0;
    const char *p = haystack;

    if (!haystack || !needle)
        return 0;
    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p += strlen(needle);
    }
    return n;
}

static bool length_matches(const char *message)
{
    const char *header = strstr(message, "Content-Length: ");
    const char *body;
    unsigned long declared;

    if (!header)
        return false;
    declared = strtoul(header + strlen("Content-Length: "), NULL, 10);
    body = strstr(message, "\r\n\r\n");
    if (!body)
        return false;
    body += 4;
    return declared == strlen(body);
}

int main(void)
{
    capture cap;
    ph_wda *wda;
    const char *body;
    int calls;

    memset(&cap, 0, sizeof cap);
    CHECK(ph_wda_create(NULL, 8100) == NULL);
    CHECK(ph_wda_create("", 8100) == NULL);
    wda = ph_wda_create("127.0.0.1", 8100);
    CHECK(wda);
    ph_wda_set_send(wda, capture_send, &cap);

    CHECK(ph_wda_tap(wda, 12, 34) == -1);
    CHECK(ph_wda_drag(wda, 1, 2, 3, 4, 0.5) == -1);
    CHECK(ph_wda_pinch(wda, 10, 10, 2) == -1);
    CHECK(cap.calls == 0);
    CHECK(ph_wda_note_response(wda, "{\"value\":{}}") == -1);
    CHECK(ph_wda_tap(wda, 1, 1) == -1);

    CHECK(ph_wda_create_session(wda) == 0);
    CHECK(cap.calls == 1);
    CHECK(strstr(cap.data, "POST /session HTTP/1.1\r\n"));
    CHECK(strstr(cap.data, "Host: 127.0.0.1:8100\r\n"));
    CHECK(strstr(cap.data, "Content-Type: application/json\r\n"));
    CHECK(length_matches(cap.data));
    body = strstr(cap.data, "\r\n\r\n");
    CHECK(body && strcmp(body + 4, "{\"capabilities\":{\"alwaysMatch\":{\"platformName\":\"iOS\"}}}") == 0);

    CHECK(ph_wda_note_response(wda, "HTTP/1.1 200 OK\r\n\r\n{\"value\":{},\"sessionId\":\"sess-1\"}") == 0);

    CHECK(ph_wda_tap(wda, 12, 34) == 0);
    CHECK(strstr(cap.data, "POST /session/sess-1/wda/tap HTTP/1.1\r\n"));
    CHECK(strstr(cap.data, "Host: 127.0.0.1:8100\r\n"));
    CHECK(length_matches(cap.data));
    body = strstr(cap.data, "\r\n\r\n");
    CHECK(body && strcmp(body + 4, "{\"x\":12,\"y\":34}") == 0);

    CHECK(ph_wda_tap(wda, 1.5, 2.25) == 0);
    body = strstr(cap.data, "\r\n\r\n");
    CHECK(body && strstr(body, "\"x\":1.5") && strstr(body, "\"y\":2.25"));

    CHECK(ph_wda_drag(wda, 10, 20, 30, 40, 0.5) == 0);
    CHECK(strstr(cap.data, "POST /session/sess-1/wda/dragfromtoforduration HTTP/1.1\r\n"));
    CHECK(length_matches(cap.data));
    body = strstr(cap.data, "\r\n\r\n");
    CHECK(body && strcmp(body + 4, "{\"fromX\":10,\"fromY\":20,\"toX\":30,\"toY\":40,\"duration\":0.5}") == 0);

    CHECK(ph_wda_drag(wda, 1, 2, 3, 4, 1) == 0);
    body = strstr(cap.data, "\r\n\r\n");
    CHECK(body && strstr(body, "\"duration\":1}") && !strstr(body, "\"duration\":1.0"));

    calls = cap.calls;
    CHECK(ph_wda_pinch(wda, 200, 400, 1) == 0);
    CHECK(cap.calls == calls);

    CHECK(ph_wda_pinch(wda, 200, 400, 0) == -1);
    CHECK(ph_wda_pinch(wda, 200, 400, -2) == -1);
    CHECK(cap.calls == calls);

    CHECK(ph_wda_pinch(wda, 200, 400, 2) == 0);
    CHECK(strstr(cap.data, "POST /session/sess-1/actions HTTP/1.1\r\n"));
    CHECK(length_matches(cap.data));
    CHECK(count_text(cap.data, "\"type\":\"pointer\",\"id\"") == 2);
    CHECK(strstr(cap.data, "\"pointerType\":\"touch\""));
    CHECK(strstr(cap.data, "\"id\":\"finger1\"") && strstr(cap.data, "\"id\":\"finger2\""));
    body = strstr(cap.data, "\r\n\r\n");
    CHECK(body && strstr(body, "\"x\":100") && strstr(body, "\"x\":300"));
    CHECK(body && strstr(body, "\"x\":0") && strstr(body, "\"x\":400"));

    cap.fail = 1;
    CHECK(ph_wda_tap(wda, 8, 9) == -1);
    cap.fail = 0;
    CHECK(ph_wda_create_session(NULL) == -1);
    CHECK(ph_wda_note_response(NULL, "{\"sessionId\":\"sess-1\"}") == -1);

    ph_wda_destroy(wda);
    ph_wda_destroy(NULL);
    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all wda checks passed\n");
    return 0;
}
