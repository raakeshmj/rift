/* rift_log.c — timestamped, colored logging with optional file output */

#include "rift_log.h"
#include "rift_config.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

/* ── Module State ─────────────────────────────────────────────────── */

static rift_log_level_t g_log_level = RIFT_LOG_INFO;
static FILE *g_log_file = NULL;
static bool g_color = true;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ANSI color codes */
static const char *level_colors[] = {
    [RIFT_LOG_TRACE] = "\033[90m", /* Gray      */
    [RIFT_LOG_DEBUG] = "\033[36m", /* Cyan      */
    [RIFT_LOG_INFO] = "\033[32m",  /* Green     */
    [RIFT_LOG_WARN] = "\033[33m",  /* Yellow    */
    [RIFT_LOG_ERROR] = "\033[31m", /* Red       */
};
static const char *color_reset = "\033[0m";

static const char *level_names[] = {
    [RIFT_LOG_TRACE] = "TRACE", [RIFT_LOG_DEBUG] = "DEBUG",
    [RIFT_LOG_INFO] = "INFO ",  [RIFT_LOG_WARN] = "WARN ",
    [RIFT_LOG_ERROR] = "ERROR",
};

/* ── Public API ───────────────────────────────────────────────────── */

void rift_log_init(rift_log_level_t level, const char *log_file) {
  g_log_level = level;

  if (log_file) {
    g_log_file = fopen(log_file, "a");
    if (!g_log_file) {
      fprintf(stderr, "[RIFT] Warning: cannot open log file '%s'\n", log_file);
    }
  }
}

void rift_log_shutdown(void) {
  pthread_mutex_lock(&g_log_mutex);
  if (g_log_file) {
    fclose(g_log_file);
    g_log_file = NULL;
  }
  pthread_mutex_unlock(&g_log_mutex);
}

void rift_log_set_level(rift_log_level_t level) { g_log_level = level; }

void rift_log_set_color(bool enabled) { g_color = enabled; }

void rift_log(rift_log_level_t level, const char *file, int line, const char *fmt,
             ...) {
  if (level < g_log_level)
    return;

  /* Get timestamp */
  struct timeval tv;
  gettimeofday(&tv, NULL);
  struct tm tm;
  localtime_r(&tv.tv_sec, &tm);

  char timestamp[32];
  snprintf(timestamp, sizeof(timestamp), "%02d:%02d:%02d.%03d", tm.tm_hour,
           tm.tm_min, tm.tm_sec, (int)(tv.tv_usec / 1000));

  /* Extract basename from file path */
  const char *basename = strrchr(file, '/');
  basename = basename ? basename + 1 : file;

  /* Format the user message */
  char msg[RIFT_LOG_MAX_MSG_LEN];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);

  pthread_mutex_lock(&g_log_mutex);

  /* Write to stderr */
  if (g_color && level < RIFT_LOG_NONE) {
    fprintf(stderr, "%s[%s %s %s:%d]%s %s\n", level_colors[level], timestamp,
            level_names[level], basename, line, color_reset, msg);
  } else {
    fprintf(stderr, "[%s %s %s:%d] %s\n", timestamp, level_names[level],
            basename, line, msg);
  }

  /* Write to file (if configured) */
  if (g_log_file) {
    fprintf(g_log_file, "[%s %s %s:%d] %s\n", timestamp, level_names[level],
            basename, line, msg);
    fflush(g_log_file);
  }

  pthread_mutex_unlock(&g_log_mutex);
}

const char *rift_log_level_str(rift_log_level_t level) {
  if (level >= RIFT_LOG_TRACE && level <= RIFT_LOG_ERROR)
    return level_names[level];
  return "UNKNOWN";
}
