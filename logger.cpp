/*
 * Copyright (c) 2020 rxi
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#include <sys/file.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include "logger.h"

#define MAX_CALLBACKS 2
#define LEVEL_MASK (LOG_NO_TIMESTAMP - 1)

static char *emptyStamp = NULL;
static int emptyStampLen = 0;
static const char *tsFormat = NULL;
typedef struct {
  log_LogFn fn;
  FILE *udata;
  int level;
  bool quiet;
  off_t lineOffset;
  bool isFile;
} Callback;

//void lockFile(bool mode, FILE *file) {
//	int fd = fileno(file), lmode = (mode) ? LOCK_EX : LOCK_UN;
//	while(true) {
//		int st = flock(lmode, fd);
//		if(st == 0)
//			return;
//		if(st != EWOULDBLOCK && st != EINTR)
//			log_fatal("Can't get lock on file descriptor %d", fd);
//	}
//}
//
static struct {
//  FILE *udata;
  log_LockFn lock;
  pid_t procPid;
  Callback callbacks[MAX_CALLBACKS];
} L;

static void (*exitProg)(int) = NULL;
FILE *stdOnList = NULL;
static const char *level_strings[] = {
  "", "", "", "WARNING!", "ERROR!", "FATAL"
};

#ifdef LOG_USE_COLOR
static const char *level_colors[] = {
  "\x1b[94m", "\x1b[36m", "\x1b[32m", "\x1b[33m", "\x1b[31m", "\x1b[35m"
};
#endif

static bool regularFile(FILE *f) {
	struct stat statbuf = {0};
	int rv = fstat(fileno(f), &statbuf);
	if(rv < 0)
		return rv;
	return (statbuf.st_mode & S_IFMT) == S_IFREG;
}

void logTimeFormat(const char *tf) {
	tsFormat = tf;
	time_t t = time(NULL);
	struct tm *tstamp = localtime(&t);
	char buf[64];
	emptyStampLen = strftime(buf, sizeof(buf), tsFormat, tstamp) + 1;
	sprintf(buf, "%*.*s", emptyStampLen, emptyStampLen, "");
	if(emptyStamp)
		free(emptyStamp);
	emptyStamp = strdup(buf);
}

static void file_callback(log_Event *ev) {
	int level = ev->level & LEVEL_MASK;
	flockfile(ev->udata);
	const char *format = ev->fmt;
	if(ev->fmt[0] == '\r') {
		if(ev->isFile) {
			if(ev->lineOffset < 0) {
				// start of bar
				ev->lineOffset = ftell(ev->udata);
			} else {
				// continue on the same line
				fseek(ev->udata, ev->lineOffset, SEEK_SET);
			}
		} else {
			if(ev->lineOffset < 0)
				ev->lineOffset = 1;
			else	// CR active
				fprintf(ev->udata, "\r");
		}
		format++;
	} else {
		// normal log message
		if(ev->lineOffset >= 0)
			fprintf(ev->udata, "\n");	// after CR delayed NL
		ev->lineOffset = -1;
	}
	if(level >= LOG_INFO) {
		pid_t tid = gettid();
		char buf[128];
		char errnoBuf[128] = "", lineBuf[256] = "";
		int k = 0;
		if(ev->time) {
			k = strftime(buf, sizeof(buf), tsFormat, ev->time);
			buf[k++] = ':';
			buf[k] = '\0';
		} else {
			strcpy(buf, emptyStamp);
			k = emptyStampLen;
		}
		if(tid != L.procPid && ev->lineOffset < 0)
			sprintf(buf + k, " Thr:%d", tid - L.procPid);
		if(level >= LOG_WARN) {
			if(ev->line > 0 && ev->file != NULL) {
				sprintf(lineBuf, " %s:%d", ev->file, ev->line);
				if(ev->lerrno)
					snprintf(errnoBuf, sizeof(errnoBuf), "errno %d: %s. ", ev->lerrno, strerror(ev->lerrno));
			}
			sprintf(lineBuf + strlen(lineBuf), " %-5s", level_strings[ev->level - 1]);
		}
		fprintf(ev->udata, "%s%s %s", buf, lineBuf, errnoBuf);
	}
	vfprintf(ev->udata, format, ev->ap);
	if(ev->lineOffset < 0 && ev->level >= LOG_INFO)
		fprintf(ev->udata, "\n");
	funlockfile(ev->udata);
//	fflush(ev->udata);
}

const char* log_level_string(int level) {
  return level_strings[level];
}

void log_set_level(int level, int idx) {
	if(idx < 0 || idx >= MAX_CALLBACKS)
		idx = 0;
	L.callbacks[idx].level = level;
}

bool log_set_quiet(FILE *log, bool mode) {
	bool rv = true;
	for (int i = 0; i < MAX_CALLBACKS; i++) {
		Callback *cb = L.callbacks + i;
		if (cb->fn && cb->udata == log) {
			rv = cb->quiet;
			cb->quiet = mode;
			break;
		}
	}
	return rv;
}

int log_add_callback(log_LogFn fn, FILE *udata, int level) {
	if(L.procPid == 0)
		L.procPid = getpid();
	for (int i = 0; i < MAX_CALLBACKS; i++) {
		if (!L.callbacks[i].fn) {
			L.callbacks[i] = (Callback ) { fn, udata, level, false, -1, false};
			L.callbacks[i].isFile = regularFile(udata);
			return i;
		}
	}
	return -1;
}

int log_add_fp(FILE *fp, int level) {
//	if(fp == stdout || fp == stderr)
//		return log_add_callback(stdout_callback, stdout, LOG_TRACE);
	if(stdOnList == NULL && (fp == stdout || fp == stderr))
		stdOnList = fp;
	if(tsFormat == NULL)
		logTimeFormat("%Y-%m-%d %H:%M:%S");
	return log_add_callback(file_callback, fp, level);
}

static void init_event(log_Event *ev, FILE *udata) {
	int noTime = ev->level & LOG_NO_TIMESTAMP;
	if(!noTime && !ev->time) {
		time_t t = time(NULL);
		ev->time = localtime(&t);
	}
	ev->udata = udata;
}
void loggerExitFunc(void (*func)(int)) {
//	void (*rv)(int) = exitProg;
	exitProg = func;
}

int log_log(int level, const char *file, int line, const char *fmt, ...) {
	log_Event ev = { .fmt = fmt, .file = file, .line = line, .level = level, .lerrno = errno }; // @suppress("Invalid arguments")

	level &= LEVEL_MASK;
	if(level >= LOG_FATAL) {
		if(stdOnList == NULL)
			log_add_fp(stderr, LOG_FATAL);
		else
			log_set_quiet(stdOnList, false);
	}
	for (int i = 0; i < MAX_CALLBACKS && L.callbacks[i].fn; i++) {
		Callback *cb = L.callbacks + i;
		if (cb->fn && level >= cb->level && !cb->quiet) {
			init_event(&ev, cb->udata);
			ev.lineOffset = cb->lineOffset;
			ev.isFile = cb->isFile;
			va_start(ev.ap, fmt);
			cb->fn(&ev);
			cb->lineOffset = ev.lineOffset;
			va_end(ev.ap);
		}
	}
	if (level >= LOG_FATAL) {
		if(exitProg)
			exitProg(-1);
		exit(-1);
	}
	if(level >= LOG_WARN && errno != 0)
		errno = 0;
	return (level >= LOG_WARN) ? -1 : 0;
}

