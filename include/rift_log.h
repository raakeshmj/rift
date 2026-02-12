/* rift_log.h — severity-filtered, timestamped logging with optional file output
 */

#ifndef RIFT_LOG_H
#define RIFT_LOG_H

#include <stdarg.h>
#include <stdbool.h>

/* ── Log Levels ───────────────────────────────────────────────────── */
typedef enum {
  RIFT_LOG_TRACE = 0,
  RIFT_LOG_DEBUG = 1,
  RIFT_LOG_INFO = 2,
  RIFT_LOG_WARN = 3,
  RIFT_LOG_ERROR = 4,
  RIFT_LOG_NONE = 5, /* Disable all logging */
} rift_log_level_t;

/* ── Compile-Time Log Level Filter ────────────────────────────────── */
#ifndef RIFT_MIN_LOG_LEVEL
#define RIFT_MIN_LOG_LEVEL RIFT_LOG_TRACE
#endif

/* ── Logging Macros ───────────────────────────────────────────────── */
#define RIFT_LOG_TRACE(fmt, ...)                                                \
  do {                                                                         \
    if (RIFT_LOG_TRACE >= RIFT_MIN_LOG_LEVEL)                                    \
      rift_log(RIFT_LOG_TRACE, __FILE__, __LINE__, fmt, ##__VA_ARGS__);          \
  } while (0)

#define RIFT_LOG_DEBUG(fmt, ...)                                                \
  do {                                                                         \
    if (RIFT_LOG_DEBUG >= RIFT_MIN_LOG_LEVEL)                                    \
      rift_log(RIFT_LOG_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__);          \
  } while (0)

#define RIFT_LOG_INFO(fmt, ...)                                                 \
  do {                                                                         \
    if (RIFT_LOG_INFO >= RIFT_MIN_LOG_LEVEL)                                     \
      rift_log(RIFT_LOG_INFO, __FILE__, __LINE__, fmt, ##__VA_ARGS__);           \
  } while (0)

#define RIFT_LOG_WARN(fmt, ...)                                                 \
  do {                                                                         \
    if (RIFT_LOG_WARN >= RIFT_MIN_LOG_LEVEL)                                     \
      rift_log(RIFT_LOG_WARN, __FILE__, __LINE__, fmt, ##__VA_ARGS__);           \
  } while (0)

#define RIFT_LOG_ERROR(fmt, ...)                                                \
  do {                                                                         \
    if (RIFT_LOG_ERROR >= RIFT_MIN_LOG_LEVEL)                                    \
      rift_log(RIFT_LOG_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__);          \
  } while (0)

/*
 * Initialize the logging subsystem.
 * level: minimum runtime log level.
 * log_file: optional file path for log output (NULL = stderr only).
 */
void rift_log_init(rift_log_level_t level, const char *log_file);

/*
 * Shut down the logging subsystem and close any open file.
 */
void rift_log_shutdown(void);

/*
 * Set the runtime log level filter.
 */
void rift_log_set_level(rift_log_level_t level);

/*
 * Enable or disable colored output.
 */
void rift_log_set_color(bool enabled);

/*
 * Core logging function — use the macros above instead.
 */
void rift_log(rift_log_level_t level, const char *file, int line, const char *fmt,
             ...) __attribute__((format(printf, 4, 5)));

/*
 * Get the string name for a log level.
 */
const char *rift_log_level_str(rift_log_level_t level);

#endif /* RIFT_LOG_H */
