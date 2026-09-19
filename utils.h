#ifndef UTILS_H_
#define UTILS_H_
#include <stdint.h>
#include <stdio.h>
#include <linux/limits.h>
#include <sys/stat.h>
#include <strings.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/time.h>
#include <time.h>
#include <fcntl.h>
#include <signal.h>
#include <limits.h>
#include <sys/stat.h>
#include <regex.h>
#include <math.h>
#include <assert.h>
#include <pwd.h>
#include <libgen.h>
#include "jobqueue.h"
#include "logger.h"

#define TIMESTAMP_SIZE			20
#define TIMESTAMP_SIZE_V		29
#define HASH_SIZE_DIGITS		16
#define MAX_DIRS_ON_PATH		128
#define UUID_SIZE               40

#define PERROR(level, fmt, ...) log_log(level, NULL, 0, fmt, ##__VA_ARGS__)
#define EXIT(rv, level, fmt, ...) {rv = log_log(level, __FILE__, __LINE__, fmt, ##__VA_ARGS__); goto end_proc;}
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define STRNCPY(a,b,len) if(memccpy(a,b,0,len) == NULL) (a)[len - 1] = 0
#define COPY_FLD(a,b) STRNCPY(a,b,sizeof(a))

#define KILO			1024
#define MEGA			(KILO * KILO)
#define GIGA			(KILO * MEGA)
typedef int64_t sqint;
#define BLANKS 		" \f\n\t\r\v"
#define NEWLINE		'\n'

#define UMASK					077		// Only owner can read/write
#define RUMASK					0022	// others can read

void *allocHashState(void);
int startBlockHash(void *hashState);
int updateBlockHash(void *hashState, const char *in_buf, int len);
uint64_t getHash(const char *buf, int len, char *outBuf);
uint64_t getBlockHash(void *hashState, char *outBuf);
uint64_t getHashStateLen(void *hstate);
void freeHashState(void *hstate);

void lower(char *str);
char *ltrim(const char *s);
void rtrim(char *s);
void rtrimChar(char *s, int c);
int isPrefix(const char *pref, const char *str);
void removeFromQuotes(char *str, const char *charsOnly);
int split(char *line, char *tokens[], int tsize, const char *delim = BLANKS, int tcount = 0);
char *formatFileSize(sqint size);
int readFD(int fd, char *buf, int bufSize);
int writeFD(int fd, char *buf, int bufSize);
int openAt(const char *fname, int mode, const char *dir = NULL, bool fatal = false);
FILE *openAt(const char *fname, const char *mode, const char *dir = NULL, bool fatal = false);
char *getFilePath(int fd, char *pbuf = NULL);
sqint getTimestamp(char *buf = NULL, struct timespec *in = NULL);
sqint getTimestamp(char *buf, struct timespec *in, int bufSize);
int sec2hours(int sec, char *buf);
sqint fileInfo(const char *dir, const char *fname, char *timestamp = NULL);
sqint fileInfo(FILE *f, char *timestamp = NULL);
sqint fileInfo(int fd, char *timestamp = NULL);
sqint checkTimestamp(const char *ts);
int getPathSeparator(char *buf = NULL);
const char *fileBasename(const char *path);
int addPathSeparator(char *path);
int getDiskIndex(const char *diskId);
sqint timestamp2str(const char *seconds, char *buf, int bufSize=TIMESTAMP_SIZE_V);
FILE *getOutput(const char *cmd);
int getOutputLine(FILE *fp, char *buf, int bufLen);
int removeLastNewline(char *line);
void hexDump(FILE *fp, void *addr, int len, const char *desc = NULL);
int findAnyChar(const char *str, const char *chars);
void normPath(const char *fname, char *path, const char *dir = NULL);
char *getArgsLine(int argc, const char * const argv[]);
bool isUUID(const char *uuid);
const char *getMountDir(const char *uuid);
int formatSize(sqint size, char *outBuf, bool roundResult = true);
bool isBlank(const char *str);
int replaceStr(char *buf, int bufSize, const char *from, const char *to);
int fileType(int fd);
//FILE *openTempFile(char *pattern);
sqint linesInFile(FILE *file, bool fromBegin = false);
int procCores();
void replaceChars(char *str, const char *findChars, const char *replaceChars);
sqint str2timestamp(const char *str, timespec *ts = NULL);
int suffixInFile(FILE *f, const char *str, char *line, int size);
int path4shell(char *path, int size);
sqint stdDeviation(sqint values[], int vsize);
int unlinkAt(const char *fname, const char *dir);
int monthNumber(const char *mname);
int findUser(const char *userId, const char **uname = NULL);
bool isACLactive(const char *path);
int excludeSysPath(char *sysPaths, int bufLen, const char *dir);
void expandPhrases(char *buf, int bufSize, const char *phrase, const char *tokens);

class StatusBar {
private:
	static const int MAX_FIELDS = 6;
	time_t	lastTime;
	int		maxElements;
	int		elements;
	int 	step;
	sqint	maxVolume;
	sqint	volume;
	char 	format[128];
	int		types[MAX_FIELDS] {0};
	int		fcount;
public:
	void show(int plusElements, sqint plusVolume, const char *fmt = NULL, ...);
	StatusBar(const char *fmt, int maxElem = 0, int showStep = 0, sqint maxSize = 0);
};

//
class TinyQueue : public SimpleQueue {
public:
	void clear() override;
	void *pop() override;
	int push(void *job) override;
	bool isFull() override;
	int getCount() override;
	TinyQueue(int qSize);
	~TinyQueue();

private:
	sqint		*rowids;
	int			size;
	int			tail{0};
	int			head{0};
};

//class tinyQueue : public JobQueue;

#endif /*UTILS_H_*/
