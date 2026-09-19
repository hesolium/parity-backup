#ifndef SRC_OS_H_
#define SRC_LINUX_H_

#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <limits.h>
#include <stdio.h>

int runSystem(const char *cmd);
void copyFiles(const char *src, const char *dst, int mask = 0);
int cleanDir(const char *dir, const char *pattern = NULL);
int setExecUmask(char *cmd, int mask);
void generateFileList(int diskIdx, const char *fpath);
FILE *generateRefreshList(bool newOnly);
int restoreACL(char *savePath, const char *wfile = NULL);
const char *restoreDirs(const char *path, int *emul, bool withLast);
int getFileOwner(char *savePath);
int getTempDirPermissions();
void genACLfile(int didx);
int execExternal(const char *cmd, FILE **out = NULL);
int openTempFD(const char *pattern, char *outPath = NULL, bool fatal = true);
FILE *filterLines(const char *pattern, const char *prefix, int count = 0);
FILE *unlinkTmp(FILE *file);
FILE *suid_popen(const char *program, const char *type);
int suid_pclose(FILE *iop);

#endif /* SRC_OS_H_ */
