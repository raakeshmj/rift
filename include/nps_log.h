/* nps_log.h — severity-filtered, timestamped logging with optional file output
 */

#ifndef NPS_LOG_H
#define NPS_LOG_H

#include <stdarg.h>
#include <stdbool.h>

/* ── Log Levels ───────────────────────────────────────────────────── */
typedef enum {
  NPS_LOG_TRACE = 0,
  NPS_LOG_DEBUG = 1,
  NPS_LOG_INFO = 2,
  NPS_LOG_WARN = 3,
  NPS_LOG_ERROR = 4,
  NPS_LOG_NONE = 5, /* Disable all logging */
} nps_log_level_t;

/* ── Compile-Time Log Level Filter ────────────────────────────────── */
#ifndef NPS_MIN_LOG_LEVEL
#define NPS_MIN_LOG_LEVEL NPS_LOG_TRACE
#endif

/* ── Logging Macros ───────────────────────────────────────────────── */
#define NPS_LOG_TRACE(fmt, ...)                                                \
  do {                                                                         \
    if (NPS_LOG_TRACE >= NPS_MIN_LOG_LEVEL)                                    \
      nps_log(NPS_LOG_TRACE, __FILE__, __LINE__, fmt, ##__VA_ARGS__);          \
  } while (0)

#define NPS_LOG_DEBUG(fmt, ...)                                                \
  do {                                                                         \
    if (NPS_LOG_DEBUG >= NPS_MIN_LOG_LEVEL)                                    \
      nps_log(NPS_LOG_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__);          \
  } while (0)

#define NPS_LOG_INFO(fmt, ...)                                                 \
  do {                                                                         \
    if (NPS_LOG_INFO >= NPS_MIN_LOG_LEVEL)                                     \
      nps_log(NPS_LOG_INFO, __FILE__, __LINE__, fmt, ##__VA_ARGS__);           \
  } while (0)

#define NPS_LOG_WARN(fmt, ...)                                                 \
  do {                                                                         \
    if (NPS_LOG_WARN >= NPS_MIN_LOG_LEVEL)                                     \
      nps_log(NPS_LOG_WARN, __FILE__, __LINE__, fmt, ##__VA_ARGS__);           \
  } while (0)

#define NPS_LOG_ERROR(fmt, ...)                                                \
  do {                                                                         \
    if (NPS_LOG_ERROR >= NPS_MIN_LOG_LEVEL)                                    \
      nps_log(NPS_LOG_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__);          \
  } while (0)

/*
 * Initialize the logging subsystem.
 * level: minimum runtime log level.
 * log_file: optional file path for log output (NULL = stderr only).
 */
void nps_log_init(nps_log_level_t level, const char *log_file);

/*
 * Shut down the logging subsystem and close any open file.
 */
void nps_log_shutdown(void);

/*
 * Set the runtime log level filter.
 */
void nps_log_set_level(nps_log_level_t level);

/*
 * Enable or disable colored output.
 */
void nps_log_set_color(bool enabled);

/*
 * Core logging function — use the macros above instead.
 */
void nps_log(nps_log_level_t level, const char *file, int line, const char *fmt,
             ...) __attribute__((format(printf, 4, 5)));

/*
 * Get the string name for a log level.
 */
const char *nps_log_level_str(nps_log_level_t level);

#endif /* NPS_LOG_H */
