#include "touch/wda.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ph_wda {
    char *host;
    int port;
    char session_id[256];
    int (*send)(void *user, const uint8_t *data, size_t len);
    void *user;
};

static bool format_number(char *dst, size_t cap, double value)
{
    int n;

    if (!(value < 1e300) || !(value > -1e300))
        return false;
    if (value < 1e9 && value > -1e9 && value == (double)(long long)value)
        n = snprintf(dst, cap, "%.0f", value);
    else
        n = snprintf(dst, cap, "%.6g", value);
    return n >= 0 && (size_t)n < cap;
}

static int ph_wda_post(ph_wda *wda, const char *path, const char *body)
{
    char header[768];
    size_t body_len, header_len, total;
    uint8_t *message;
    int n, rc;

    if (!wda || !wda->send || !wda->host || !path || !body)
        return -1;
    body_len = strlen(body);
    n = snprintf(header, sizeof header,
                 "POST %s HTTP/1.1\r\n"
                 "Host: %s:%d\r\n"
                 "Content-Type: application/json\r\n"
                 "Content-Length: %zu\r\n"
                 "\r\n",
                 path, wda->host, wda->port, body_len);
    if (n < 0 || (size_t)n >= sizeof header)
        return -1;
    header_len = (size_t)n;
    total = header_len + body_len;
    message = malloc(total);
    if (!message)
        return -1;
    memcpy(message, header, header_len);
    memcpy(message + header_len, body, body_len);
    rc = wda->send(wda->user, message, total);
    free(message);
    return rc == 0 ? 0 : -1;
}

static int session_path(const ph_wda *wda, char *path, size_t cap, const char *suffix)
{
    int n;

    if (!wda || wda->session_id[0] == '\0')
        return -1;
    n = snprintf(path, cap, "/session/%s%s", wda->session_id, suffix);
    if (n < 0 || (size_t)n >= cap)
        return -1;
    return 0;
}

ph_wda *ph_wda_create(const char *host, int port)
{
    ph_wda *wda;
    size_t n;

    if (!host || host[0] == '\0')
        return NULL;
    wda = calloc(1, sizeof *wda);
    if (!wda)
        return NULL;
    n = strlen(host);
    wda->host = malloc(n + 1);
    if (!wda->host) {
        free(wda);
        return NULL;
    }
    memcpy(wda->host, host, n + 1);
    wda->port = port;
    return wda;
}

void ph_wda_destroy(ph_wda *wda)
{
    if (!wda)
        return;
    free(wda->host);
    free(wda);
}

void ph_wda_set_send(ph_wda *wda, int (*send)(void *user, const uint8_t *data, size_t len),
                     void *user)
{
    if (!wda)
        return;
    wda->send = send;
    wda->user = user;
}

int ph_wda_create_session(ph_wda *wda)
{
    return ph_wda_post(wda, "/session",
                       "{\"capabilities\":{\"alwaysMatch\":{\"platformName\":\"iOS\"}}}");
}

int ph_wda_note_response(ph_wda *wda, const char *http_body)
{
    const char *mark = "\"sessionId\":\"";
    const char *start, *end;
    size_t n, i;

    if (!wda || !http_body)
        return -1;
    start = strstr(http_body, mark);
    if (!start)
        return -1;
    start += strlen(mark);
    end = strchr(start, '"');
    if (!end || end == start)
        return -1;
    n = (size_t)(end - start);
    if (n >= sizeof wda->session_id)
        return -1;
    for (i = 0; i < n; i++) {
        if (start[i] == '\r' || start[i] == '\n')
            return -1;
    }
    memcpy(wda->session_id, start, n);
    wda->session_id[n] = '\0';
    return 0;
}

int ph_wda_tap(ph_wda *wda, double x, double y)
{
    char path[320];
    char body[160];
    char xs[64], ys[64];
    int n;

    if (session_path(wda, path, sizeof path, "/wda/tap") != 0)
        return -1;
    if (!format_number(xs, sizeof xs, x) || !format_number(ys, sizeof ys, y))
        return -1;
    n = snprintf(body, sizeof body, "{\"x\":%s,\"y\":%s}", xs, ys);
    if (n < 0 || (size_t)n >= sizeof body)
        return -1;
    return ph_wda_post(wda, path, body);
}

int ph_wda_drag(ph_wda *wda, double x1, double y1, double x2, double y2, double seconds)
{
    char path[360];
    char body[256];
    char a[64], b[64], c[64], d[64], t[64];
    int n;

    if (session_path(wda, path, sizeof path, "/wda/dragfromtoforduration") != 0)
        return -1;
    if (!format_number(a, sizeof a, x1) || !format_number(b, sizeof b, y1)
        || !format_number(c, sizeof c, x2) || !format_number(d, sizeof d, y2)
        || !format_number(t, sizeof t, seconds))
        return -1;
    n = snprintf(body, sizeof body,
                 "{\"fromX\":%s,\"fromY\":%s,\"toX\":%s,\"toY\":%s,\"duration\":%s}", a, b, c, d,
                 t);
    if (n < 0 || (size_t)n >= sizeof body)
        return -1;
    return ph_wda_post(wda, path, body);
}

int ph_wda_pinch(ph_wda *wda, double cx, double cy, double scale)
{
    char path[320];
    char body[2048];
    char sx1[64], sy1[64], sx2[64], sy2[64], ex1[64], ex2[64];
    const double half = 100.0;
    int n;

    if (scale == 1.0)
        return 0;
    if (!(scale > 0.0))
        return -1;
    if (session_path(wda, path, sizeof path, "/actions") != 0)
        return -1;
    if (!format_number(sx1, sizeof sx1, cx - half) || !format_number(sy1, sizeof sy1, cy)
        || !format_number(sx2, sizeof sx2, cx + half) || !format_number(sy2, sizeof sy2, cy)
        || !format_number(ex1, sizeof ex1, cx - half * scale)
        || !format_number(ex2, sizeof ex2, cx + half * scale))
        return -1;
    n = snprintf(body, sizeof body,
                 "{\"actions\":["
                 "{\"type\":\"pointer\",\"id\":\"finger1\",\"parameters\":{\"pointerType\":\"touch\"},"
                 "\"actions\":["
                 "{\"type\":\"pointerMove\",\"duration\":0,\"x\":%s,\"y\":%s,\"origin\":\"viewport\"},"
                 "{\"type\":\"pointerDown\",\"button\":0},"
                 "{\"type\":\"pointerMove\",\"duration\":250,\"x\":%s,\"y\":%s,\"origin\":\"viewport\"},"
                 "{\"type\":\"pointerUp\",\"button\":0}]},"
                 "{\"type\":\"pointer\",\"id\":\"finger2\",\"parameters\":{\"pointerType\":\"touch\"},"
                 "\"actions\":["
                 "{\"type\":\"pointerMove\",\"duration\":0,\"x\":%s,\"y\":%s,\"origin\":\"viewport\"},"
                 "{\"type\":\"pointerDown\",\"button\":0},"
                 "{\"type\":\"pointerMove\",\"duration\":250,\"x\":%s,\"y\":%s,\"origin\":\"viewport\"},"
                 "{\"type\":\"pointerUp\",\"button\":0}]}"
                 "]}",
                 sx1, sy1, ex1, sy1, sx2, sy2, ex2, sy2);
    if (n < 0 || (size_t)n >= sizeof body)
        return -1;
    return ph_wda_post(wda, path, body);
}
