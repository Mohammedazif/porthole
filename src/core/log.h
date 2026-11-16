#ifndef PORTHOLE_CORE_LOG_H
#define PORTHOLE_CORE_LOG_H

/* Minimal leveled logger writing to stderr. Each message is written with a single
 * stdio call, so lines from different threads don't interleave. Timestamps count
 * from the first log call, which should happen on the main thread before worker
 * threads start. All Porthole symbols use the ph_/PH_ prefix to avoid clashing
 * with UxPlay's own logger. */

#if defined(__GNUC__) || defined(__clang__)
#define PH_PRINTF_FMT(fmt_idx, args_idx) __attribute__((format(printf, fmt_idx, args_idx)))
#else
#define PH_PRINTF_FMT(fmt_idx, args_idx)
#endif

typedef enum ph_log_level {
    PH_LOG_DEBUG = 0,
    PH_LOG_INFO,
    PH_LOG_WARN,
    PH_LOG_ERROR,
} ph_log_level;

void ph_log_set_level(ph_log_level level);
ph_log_level ph_log_get_level(void);

void ph_log(ph_log_level level, const char *fmt, ...) PH_PRINTF_FMT(2, 3);

#define PH_LOGD(...) ph_log(PH_LOG_DEBUG, __VA_ARGS__)
#define PH_LOGI(...) ph_log(PH_LOG_INFO, __VA_ARGS__)
#define PH_LOGW(...) ph_log(PH_LOG_WARN, __VA_ARGS__)
#define PH_LOGE(...) ph_log(PH_LOG_ERROR, __VA_ARGS__)

#endif
