#define _POSIX_C_SOURCE 200809L // clock_gettime, setlinebuf under -std=c11

#include "logger.h"

#include "arena.h"
#include "string.h"

// TODO: temporary
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static log_frame* g_current_log_frame = NULL;
static pthread_mutex_t g_frame_mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE* g_log_file = NULL;
static char g_crash_dump_path[1024] = {0};

static const char* k_level_prefix[LOG_LEVEL_COUNT] = {
    "[FATAL]",
    "[ERROR]",
    "[WARNING]",
    "[INFO]",
    "[DEBUG]",
    "[TRACE]",
};

// ANSI colors — the platform layer of the engine this logger came from
// mapped the colour ids; here we keep the codes directly.
static const char* k_level_ansi[LOG_LEVEL_COUNT] = {
    "\033[1;31m", // FATAL
    "\033[31m",   // ERROR
    "\033[33m",   // WARNING
    "\033[32m",   // INFO
    "\033[36m",   // DEBUG
    "\033[90m",   // TRACE
};

static f64 log_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (f64)ts.tv_sec + (f64)ts.tv_nsec / 1e9;
}

static void format_wall_time(char* buf, u64 size) {
  time_t now = time(NULL);
  struct tm* tm = localtime(&now);
  if (tm) {
    strftime(buf, size, "%Y-%m-%d %H:%M:%S", tm);
  } else {
    snprintf(buf, size, "unknown");
  }
}

void log_system_init(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  setvbuf(stderr, NULL, _IOLBF, 0);
}

void log_set_file_output(const char* path) {
  pthread_mutex_lock(&g_frame_mutex);
  if (g_log_file) {
    fclose(g_log_file);
    g_log_file = NULL;
  }
  if (path) {
    g_log_file = fopen(path, "a");
  }
  pthread_mutex_unlock(&g_frame_mutex);
}

void log_set_crash_dump_path(const char* path) {
  pthread_mutex_lock(&g_frame_mutex);
  if (path) {
    snprintf(g_crash_dump_path, sizeof(g_crash_dump_path), "%s", path);
  } else {
    g_crash_dump_path[0] = 0;
  }
  pthread_mutex_unlock(&g_frame_mutex);
}

void log_set_current_frame(log_frame* frame) {
  pthread_mutex_lock(&g_frame_mutex);
  g_current_log_frame = frame;
  pthread_mutex_unlock(&g_frame_mutex);
}

log_frame logframe_init(MemArena* arena, u64 capacity) {
  return (log_frame){
      .arena = arena,
      .capacity = capacity,
      .start_pos = ARENA_SAVE_POS(arena),
      .count = 0,
      .log = PUSH_ARRAY_ZERO(arena, log_massage, capacity),
  };
}

void logframe_invalidate(log_frame* frame) {
  if (!frame || !frame->arena) {
    return;
  }
  pthread_mutex_lock(&g_frame_mutex);
  if (g_current_log_frame == frame) {
    g_current_log_frame = NULL;
  }
  pthread_mutex_unlock(&g_frame_mutex);
  ARENA_RESTORE(frame->arena, frame->start_pos);
  *frame = (log_frame){0};
}

b8 logframe_push(log_frame* frame, log_level level, String8 message) {
  if (!frame || frame->count >= frame->capacity) {
    return FALSE;
  }
  log_massage* msg = &frame->log[frame->count++];
  msg->level = level;
  msg->time_stamp = log_now();
  msg->massage = str8_copy(frame->arena, message);
  return TRUE;
}

void log_output(log_level level, const char* fmt, ...) {
  b8 is_error = level < LOG_LEVEL_WARNING;

  String8 body = {0};
  log_frame* frame = NULL;
  pthread_mutex_lock(&g_frame_mutex);
  frame = g_current_log_frame;
  if (frame) {
    va_list args;
    va_start(args, fmt);
    body = str8_pushfv(frame->arena, fmt, args);
    va_end(args);
    logframe_push(frame, level, body);
  }
  pthread_mutex_unlock(&g_frame_mutex);

  if (!frame) {
    // no frame active (e.g. very early startup) — fall back to a small stack buf
    char tmp[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);
    body = str8(tmp);
  }

  char line[600]; // only need this for the prefixed console line
  snprintf(line, sizeof(line), "%s %.*s", k_level_prefix[level], str8_fmt(body));
  // note: if body is longer than ~590 chars this truncates only what is
  // printed to console; with an active frame the full string is still
  // safely in the frame's arena

  if (g_log_file) {
    pthread_mutex_lock(&g_frame_mutex);
    if (g_log_file) {
      char ts[64];
      format_wall_time(ts, sizeof(ts));
      fprintf(g_log_file, "[%s] %s %.*s\n", ts, k_level_prefix[level], str8_fmt(body));
      fflush(g_log_file);
    }
    pthread_mutex_unlock(&g_frame_mutex);
  }

  if (is_error) {
    fprintf(stderr, "%s%s\033[0m\n", k_level_ansi[level], line);
  } else {
    fprintf(stdout, "%s%s\033[0m\n", k_level_ansi[level], line);
  }

  if (level == LOG_LEVEL_FATAL) {
    logframe_dump(frame, g_crash_dump_path);
  }
}

b8 logframe_dump(log_frame* frame, const char* path) {
  if (!frame || !frame->arena || !path || !path[0]) {
    return FALSE;
  }
  pthread_mutex_lock(&g_frame_mutex);
  FILE* f = fopen(path, "w");
  if (!f) {
    pthread_mutex_unlock(&g_frame_mutex);
    return FALSE;
  }
  f64 first_ts = (frame->count > 0) ? frame->log[0].time_stamp : 0;
  fprintf(f, "--- log frame dump (count=%llu) ---\n", (unsigned long long)frame->count);
  for (u64 i = 0; i < frame->count; i++) {
    log_massage* msg = &frame->log[i];
    fprintf(f, "[%+.6f] %s %.*s\n", msg->time_stamp - first_ts, k_level_prefix[msg->level],
            str8_fmt(msg->massage));
  }
  fprintf(f, "--- end dump ---\n");
  fclose(f);
  pthread_mutex_unlock(&g_frame_mutex);
  return TRUE;
}

void report_assertion_failure(const char* expression, const char* message, const char* file,
                              u32 line) {
  log_output(LOG_LEVEL_FATAL, "Assertion Failure: %s, message: '%s', in file: %s, line: %d",
             expression, message, file, line);
}
