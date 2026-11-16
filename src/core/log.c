#include "core/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdatomic.h>

#include "core/metrics.h"

static atomic_int g_level = PH_LOG_INFO;

void ph_log_set_level(ph_log_level level)
{
    atomic_store(&g_level, level);
}

ph_log_level ph_log_get_level(void)
{
    return (ph_log_level)atomic_load(&g_level);
}

/* Seconds since the first log call, with millisecond precision. */
static double elapsed_seconds(void)
{
    static atomic_uint_fast64_t start;
    uint64_t now = ph_metrics_now_ms();
    uint_fast64_t unset = 0;
    atomic_compare_exchange_strong(&start, &unset, now);
    uint64_t first = atomic_load(&start);
    return now >= first ? (double)(now - first) / 1000 : 0;
}

void ph_log(ph_log_level level, const char *fmt, ...)
{
    static const char tags[] = {'D', 'I', 'W', 'E'};
    char line[1024];
    int n;
    va_list ap;

    if (level < ph_log_get_level() || level > PH_LOG_ERROR)
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
