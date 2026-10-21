#pragma once

#include <stdarg.h>

#include "types.h"

typedef struct MemArena MemArena;

#define LOG_WARNING_ENABLE 1
#define LOG_INFO_ENABLE 1
#define LOG_DEBUG_ENABLE 1
#define LOG_TRACE_ENABLE 1

#if T_RELEASE == 1
  #undef LOG_DEBUG_ENABLE
  #define LOG_DEBUG_ENABLE 0
  #undef LOG_TRACE_ENABLE
  #define LOG_TRACE_ENABLE 0
#endif

typedef enum log_level {
  LOG_LEVEL_FATAL = 0,
  LOG_LEVEL_ERROR = 1,
  LOG_LEVEL_WARNING,
  LOG_LEVEL_INFO,
  LOG_LEVEL_DEBUG,
  LOG_LEVEL_TRACE,
  LOG_LEVEL_COUNT
} log_level;

typedef struct log_massage log_massage;
struct log_massage {
  log_level level;
  u32 scope_depth;
  f64 time_stamp;
  String8 massage;
};

typedef struct log_frame log_frame;
struct log_frame {
  MemArena* arena;
  log_massage* log;
  u64 start_pos;
  u64 capacity;
  u64 count;
};

log_frame logframe_init(MemArena* arena, u64 capacity);
void logframe_invalidate(log_frame* frame);
b8 logframe_push(log_frame* frame, log_level level, String8 message);
b8 logframe_dump(log_frame* frame, const char* path);

void log_system_init(void); // one-time, call at startup — line buffering etc
void log_set_current_frame(log_frame* frame);
void log_set_file_output(const char* path); // tee every log line to this file (append)
void log_set_crash_dump_path(const char* path); // on LOG_LEVEL_FATAL, dump current frame here
void log_output(log_level level, const char* fmt, ...);

void report_assertion_failure(const char* expression, const char* message,
                              const char* file, u32 line);

#define T_FATAL(msg, ...) log_output(LOG_LEVEL_FATAL, msg, ##__VA_ARGS__)
#define T_ERROR(msg, ...) log_output(LOG_LEVEL_ERROR, msg, ##__VA_ARGS__)

#if LOG_WARNING_ENABLE == 1
  #define T_WARNING(msg, ...) log_output(LOG_LEVEL_WARNING, msg, ##__VA_ARGS__)
#else
  #define T_WARNING(msg, ...) \
    do {                      \
    } while (0)
#endif

#if LOG_INFO_ENABLE == 1
  #define T_INFO(msg, ...) log_output(LOG_LEVEL_INFO, msg, ##__VA_ARGS__)
#else
  #define T_INFO(msg, ...) \
    do {                   \
    } while (0)
#endif

#if LOG_DEBUG_ENABLE == 1
  #define T_DEBUG(msg, ...) log_output(LOG_LEVEL_DEBUG, msg, ##__VA_ARGS__)
#else
  #define T_DEBUG(msg, ...) \
    do {                    \
    } while (0)
#endif

#if LOG_TRACE_ENABLE == 1
  #define T_TRACE(msg, ...) log_output(LOG_LEVEL_TRACE, msg, ##__VA_ARGS__)
#else
  #define T_TRACE(msg, ...) \
    do {                    \
    } while (0)
#endif
