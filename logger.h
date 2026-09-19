/**
 * Copyright (c) 2020 rxi
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the MIT license. See `log.c` for details.
 */

#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

#define LOG_VERSION "0.1.0"

typedef struct {
  va_list ap;
  const char *fmt;
  const char *file;
  struct tm *time;
  FILE *udata;
  int line;
  int level;
  int lerrno;
  off_t lineOffset;
  bool isFile;
} log_Event;

typedef void (*log_LogFn)(log_Event *ev);
typedef void (*log_LockFn)(bool lock, FILE *file);
#define LOG_NO_TIMESTAMP	16
#define LOG_INFO_NOTIME (LOG_INFO + LOG_NO_TIMESTAMP)
enum { LOG_TRACE = 1, LOG_DEBUG = 2, LOG_INFO = 3, LOG_WARN = 4, LOG_ERROR = 5, LOG_FATAL = 6};

#define log_trace(...) log_log(LOG_TRACE, __FILE__, __LINE__, __VA_ARGS__)
#define log_debug(...) log_log(LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define log_info(...)  log_log(LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define log_notime(...)  log_log(LOG_INFO_NOTIME,  __FILE__, __LINE__, __VA_ARGS__)
#define log_warn(...)  log_log(LOG_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define log_error(...) log_log(LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define log_fatal(...) log_log(LOG_FATAL, __FILE__, __LINE__, __VA_ARGS__)

void loggerExitFunc(void (*func)(int));

const char* log_level_string(int level);
//void log_set_lock(log_LockFn fn, void *udata);
void log_set_level(int level, int idx);
bool log_set_quiet(FILE *log, bool mode);
int log_add_callback(log_LogFn fn, void *udata, int level);
int log_add_fp(FILE *fp, int level);
void logTimeFormat(const char *tf);
int log_log(int level, const char *file, int line, const char *fmt, ...);
//void lockFile(bool mode, FILE *file);

#endif
