#include "core/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static volatile ph_log_level g_level = PH_LOG_INFO;

void ph_log_set_level(ph_log_level level)
{
    g_level = level;
}

ph_log_level ph_log_get_level(void)
{
    return g_level;
}

/* Seconds since the first log call, with millisecond precision. */
static double elapsed_seconds(void)
{
    static struct timespec start;
    static int started;
    struct timespec now;

    if (!timespec_get(&now, TIME_UTC))
        return 0.0;
    if (!started) {
        start = now;
        started = 1;
    }
    return (double)(now.tv_sec - start.tv_sec) + (double)(now.tv_nsec - start.tv_nsec) / 1e9;
}

void ph_log(ph_log_level level, const char *fmt, ...)
{
    static const char tags[] = {'D', 'I', 'W', 'E'};
    char line[1024];
    int n;
    va_list ap;

    if (level < g_level || level > PH_LOG_ERROR)
        return;

    n = snprintf(line, sizeof line, "[%9.3f] %c ", elapsed_seconds(), tags[level]);
    if (n < 0)
        return;

    va_start(ap, fmt);
    vsnprintf(line + n, sizeof line - (size_t)n, fmt, ap);
    va_end(ap);

    /* Everything goes to stderr so messages stay in order when output is piped. */
    fprintf(stderr, "%s\n", line);
}
