#include "os.h"
#include "logger.h"
#include "utils.h"
#include "backup.h"

extern CONFIG config;
//	Option for Linux 'find' command
const char *path_exclude_option = "-path $DISK$PATH -prune -o";
const char *name_exclude_option = "! -name $PATH";
const char *regex_exclude_option = "! -regex $PATH";
const char *filter = "find $DISK -mount -readable $EXCLUDES -printf '%s %y %T@ %U %i %p\n' 2>$STDERR | sort -k1nr -k5";

void generateFileList(int diskIdx, const char *fpath) {
	char *diskId = config.diskDesc[diskIdx].id;
	const char *dpath = config.diskDesc[diskIdx].path;
	char cmd[2 * PATH_MAX], sysPaths[128] = {0}, phrase[PATH_MAX] = "";

	setExecUmask(cmd, UMASK);
	strcat(cmd, filter);
	int k = excludeSysPath(sysPaths, sizeof(sysPaths), DEFAULT_CONFIG_DIR);
	expandPhrases(phrase, sizeof(phrase), path_exclude_option, sysPaths + k);
	k = excludeSysPath(sysPaths, sizeof(sysPaths), DEFAULT_BACKUP_SUBDIR);
	expandPhrases(phrase, sizeof(phrase), path_exclude_option, sysPaths + k);
	k = excludeSysPath(sysPaths, sizeof(sysPaths), TEMP_SUBDIR);
	expandPhrases(phrase, sizeof(phrase), path_exclude_option, sysPaths + k);
	expandPhrases(phrase, sizeof(phrase), path_exclude_option, config.excludePaths);
	expandPhrases(phrase, sizeof(phrase), name_exclude_option, config.excludeNames);
	expandPhrases(phrase, sizeof(phrase), regex_exclude_option, config.excludeRegex);
	if(replaceStr(cmd, sizeof(cmd), "$EXCLUDES", phrase) < 0)
		log_fatal("Too big external command: %.200s", cmd);
	if(replaceStr(cmd, sizeof(cmd), "$DISK", dpath) < 0)
		log_fatal("Too big external command: %.200s", cmd);
	sprintf(cmd + strlen(cmd), " > %s ", fpath);
	log_info("Prepare file list for disk %s [%s] ... ", diskId, dpath);
	unlink(fpath);
	execExternal(cmd);
}

int openTempFD(const char *pattern, char *outPath, bool fatal) {
	char wpath[PATH_MAX];
	const char *p, *fdir = (config.cmd == RESTORE) ? getRestorePath() : config.workDir;
	sprintf(wpath, "%s%s%s", fdir, TEMP_SUBDIR, pattern);
	p = strstr(pattern, "XXXXXX");
	if(p == NULL) {
		p = wpath + strlen(wpath) + 1;
		strcat(wpath, "-XXXXXX");
	}
	int fd = mkstemps(wpath, strlen(p + 6));
	if(fd < 0 && fatal)
		PERROR(LOG_FATAL, "Can't create temporary file %s", wpath);
	if(outPath)
		strcpy(outPath, wpath);
	return fd;
}

FILE *openTempFile(const char *pattern) {
	int fd = openTempFD(pattern);
	FILE *rv = fdopen(fd, "r+");
	return rv;
}

// Run external program. Save 'stderr' in case of errors or warning
int execExternal(const char *cmd, FILE **out) {
	char errFile[PATH_MAX];
	int k, fd = openTempFD("run-XXXXXX.stderr", errFile);
	close(fd);
	char wrk[PATH_MAX];
	strcpy(wrk, cmd);
	if(strstr(wrk, "$STDERR"))
		replaceStr(wrk, sizeof(wrk), "$STDERR", errFile);
	else {
		strcat(wrk, " 2>");
		strcat(wrk, errFile);
	}
	if(out) {
		strcpy(errFile + strlen(errFile) - 6, "stdout");
		strcat(wrk, " 1>");
		strcat(wrk, errFile);
	}
	int rv = runSystem(wrk);
	strcpy(errFile + strlen(errFile) - 6, "stderr");
	k = fileInfo(NULL, errFile, NULL);
	if(k > 0 || rv != 0) {
		// warning or errors. Append to log file
		if(k > 0) {
			char msg[8 * KILO] = {0};
			int size = sprintf(msg, "stderr of program '%s'\n", cmd);
			fd = open(errFile, O_RDONLY);
			if(fd >= 0) {
				readFD(fd, msg + size, sizeof(msg) - size - 1);
				close(fd);
			} else {
				sprintf(msg, "Can't read file: %s", errFile);
				rv = -1;
			}
			log_log(LOG_WARN, NULL, 0, "%s", msg);
		}
		if(rv < 0)
			log_log(LOG_FATAL, NULL, 0, "Can't run cmd '%s' ", cmd);
	}
	if(out) {
		strcpy(wrk, errFile);
		strcpy(wrk + strlen(wrk) - 6, "stdout");
		*out = openAt(wrk, "r+", NULL, true);
	}
	unlink(errFile);
	return rv;
}

void genACLfile(int didx) {
	char cmd[PATH_MAX];
	const char *dpath = config.diskDesc[didx].path;
	int k = sprintf(cmd, "umask 0%o; echo 'D%d=%s' >%sD%d.acl; ", UMASK, didx + 1, dpath, config.workDir, didx + 1);
	k += sprintf(cmd + k, "cat %sD%dacl.list | xargs getfacl -n -p >>%sD%d.acl",
		config.workDir, didx + 1, config.workDir, didx + 1);
	if(execExternal(cmd) == 0) {
		sprintf(cmd, "%sD%dacl.list", config.workDir, didx + 1);
		unlink(cmd);
	}
}

FILE *getOutput(const char *cmd) {
	FILE *resp = suid_popen(cmd, "r");
	if(resp == NULL)
		log_fatal("Run command '%s'", cmd);
	return resp;
}

int runSystem(const char *cmd) {
	int rv = 0;
	int child = fork();
	if(child < 0)
		log_fatal("Can't fork");
	if (child == 0) {
		//	child process
		uid_t uid, euid, sid;
		getresuid(&uid, &euid, &sid);
		if(uid != euid) {
			sid = getegid();
			rv = setregid(sid, sid);
			rv += setreuid(euid, euid);
		}
		execle("/bin/bash", "bash", "-c", cmd, (char *) NULL, environ);
	} else {
		int st = waitpid(child, &rv, WUNTRACED | WCONTINUED);	// Parent wait
		if(st <= 0 || !WIFEXITED(rv)) {
			log_fatal("External program '%s' exit abnormally", cmd);
		} else
			rv = WEXITSTATUS(rv);
	}
	return rv;
}

int setExecUmask(char *cmd, int mask) {
	if(mask)
		return sprintf(cmd, "umask 0%o; ", mask);
	return 0;
}

void copyFiles(const char *src, const char *dst, int mask) {
	char cmd[PATH_MAX + 128];
	int	k = setExecUmask(cmd, mask);
	sprintf(cmd + k, "cp %s %s 2> /dev/null", src, dst);
	int rv = runSystem(cmd);
	if(rv != 0)
		log_error("Some files not copied to '%s'", dst);
}

char *getFilePath(int fd, char *pbuf) {
	char ln[64];
	snprintf(ln, sizeof(ln), "/proc/self/fd/%d", fd);
	char *rv = realpath(ln, pbuf);
	return rv;
}

FILE *unlinkTmp(FILE *file) {
	if(file) {
		char fname[PATH_MAX];
		if(getFilePath(fileno(file), fname))
			unlink(fname);
		fclose(file);
	}
	return NULL;
}

FILE *filterLines(const char *files, const char *pattern, int count) {
	char buf[PATH_MAX];
	int	k = setExecUmask(buf, UMASK);
	FILE *sfile = NULL;
	k += snprintf(buf + k, sizeof(buf) - k, "grep -s --color=never ");
	if(count)
		k += snprintf(buf + k, sizeof(buf) - k, "-a -m %d ", count);
	k += snprintf(buf + k, sizeof(buf) - k, "'%s' ", pattern);
	strcpy(buf + k, files);
	sfile = getOutput(buf);
//	int rv = execExternal(buf, &sfile);
//	if(rv != 0)
//		sfile = unlinkTmp(sfile);
	return sfile;
}

char *pathInFile(FILE *f, const char *str, char line[PATH_MAX]) {
//	if(fseek(f, 0, SEEK_SET) != 0)
//		return log_error("Can't seek on file");
//	int slen = strlen(str), llen;
	char *p;
	while(fgets(line, PATH_MAX, f)) {
		if(!isPrefix("# file:", line))
			continue;
		removeLastNewline(line);
		p = strchr(line, getPathSeparator());
		if(p == NULL)
			continue;
		if(strcmp(p, str) == 0)
			return p;
	}
	return NULL;
}

// Open ACL file and position to ACL for savePath
FILE *locateInACL(char *savePath, char diskSubst[PATH_MAX], char **spath) {
	char aclName[32], *pathPrefix, *fpath;
	int idx = getDiskIndex(savePath);
	char *diskId = config.diskDesc[idx].id, pathSep = getPathSeparator();
	sprintf(aclName, "%s.acl", diskId);
	FILE *f = openAt(aclName, "r", config.workDir, true);
	if(fgets(diskSubst, PATH_MAX, f) == NULL) {
		PERROR(LOG_ERROR, "Can't read ACL file %s", aclName);
		fclose(f);
		return NULL;
	}
	sprintf(aclName, "%s=%c", diskId, getPathSeparator());
	if(!isPrefix(aclName, diskSubst)) {
		PERROR(LOG_ERROR, "ACL improper header line %s", diskSubst);
		fclose(f);
		return NULL;
	}
	removeLastNewline(diskSubst);
	pathPrefix = diskSubst + strlen(aclName) - 1;
	fpath = strchr(savePath, pathSep);
	strcat(pathPrefix, fpath + 1);
	char prefix[strlen(pathPrefix) + 1];
	strcpy(prefix, pathPrefix);
	fpath = pathInFile(f, prefix, diskSubst);
	if(fpath == NULL)
		goto end_proc;
	if(spath)
		*spath = fpath;
	return f;
end_proc:
	fclose(f);
	return NULL;
}

int cleanDir(const char *dir, const char *pattern) {
	struct stat statbuf = {0};
	int rv = stat(dir, &statbuf);
	if(rv == 0) {
		if((statbuf.st_mode & S_IFMT) != S_IFDIR)
			log_fatal("Path is not directory %s", dir);
		char cmd[PATH_MAX + 128], wrk[256] = "";
		if(pattern)
			sprintf(wrk, "-name '%s'", pattern);
		sprintf(cmd, "find %s -maxdepth 1 -type f %s -delete", dir, wrk);
		if(runSystem(cmd) != 0)
			log_fatal("Unable remove files from directory %s", dir);
	}
	return rv;
}

int getFileOwner(char *savePath) {
	int rv = -1;
	char buffer[PATH_MAX] = "", *p;
	FILE *f = locateInACL(savePath, buffer, NULL);
	if(f) {
		// next line is '# owner : nnnn'
		if(fgets(buffer, PATH_MAX, f)) {
			if(isPrefix("# owner:", buffer)) {
				p = strchr(buffer, ':') + 1;
				rv = strtol(p, NULL, 0);
			}
		}
		if(rv < 0)
			PERROR(LOG_ERROR, "ACL improper format. next line should be '# owner:'. Is: '%s'", buffer);
		fclose(f);
	}
	return rv;
}

FILE *generateRefreshList(bool newOnly) {
	char line[PATH_MAX];
	strcpy(line, PARITY_DIR);
	int k = path4shell(line, sizeof(line));
	char fileName[k + 128], ppath[k + 1];
	strcpy(ppath, line);
	k = setExecUmask(line, RUMASK);
	k += sprintf(line + k, "find %s -maxdepth 1 -type f -regextype egrep ", ppath);
	if(newOnly)
		k += sprintf(line + k, " -newer %s%s ", ppath, SYNC_START_MARKER);
	k += sprintf(line + k, "-regex '.*%s[0-9abcdefgh]{3}-[0-9]{10}\\.[0-9]{9}' ", SLICE_PREFIX);
	sprintf(fileName, "%s%s", ppath, LAST_GENER);
	strcpy(line + k, " -printf '%P %T@\n' | sort -k2 >");
	strcat(line + k, fileName);
	execExternal(line);
	FILE *resp = openAt(LAST_GENER, "r", PARITY_DIR, true);
	if(resp == NULL)
		log_fatal("Create list of slices to refresh failed: %s", fileName);
	return resp;
}

int restoreACL(char *savePath, const char *wfile) {
	char line[PATH_MAX + 512], *fpath, buf[64], tmpname[PATH_MAX];
	FILE *f = locateInACL(savePath, line, &fpath);
	if(f == NULL)
		return -1;
	// write to temporary file
	int wrk, rv = 0, fowner = -1, fgroup = -1, user = config.euserId;
	strcpy(tmpname, "tmp-XXXXXX.acl");
	FILE *tmp = openTempFile(tmpname);
	// replace target file name
	getFilePath(fileno(tmp), tmpname);
	if(wfile)
		strcpy(fpath, wfile);
	fprintf(tmp, "%s\n", line);
//# file: full path
//# owner: 1000
//# group: 1000
//user::rw-
//group::r--
//other::r--
	while(fgets(line, sizeof(line), f)) {
		if(isPrefix("# file:", line))
			break;
		if(isPrefix("# owner:", line)) {
			fpath = strchr(line, ':') + 1;
			wrk = strtol(fpath, NULL, 0);
			if(user > 0 && wrk != user) {	// normal user and not owner
				fowner = wrk;
				rv = -1;
				continue;	// skip set file owner
			}
		}
		else if(isPrefix("# group:", line)) {
			fpath = strchr(line, ':') + 1;
			wrk = strtol(fpath, NULL, 0);
			if(user > 0 && fowner > 0) {	// normal user and not owner
				fgroup = wrk;
				rv = -1;
				continue;	// skip set file group
			}
		}
		else if(isPrefix("user::", line) && fowner > 0) {
			fprintf(tmp, "%s", line);
			sprintf(buf, ":%d:", fowner);
			replaceStr(line, sizeof(line), "::", buf);
		}
		else if(isPrefix("group::", line) && fgroup > 0) {
			fprintf(tmp, "%s", line);
			sprintf(buf, ":%d:", fgroup);
			replaceStr(line, sizeof(line), "::", buf);
		} else if(!isPrefix("other::", line)) {
			if((fowner > 0) && !config.restoreACLsupported)
				continue;	// not file owner.
		}
		fprintf(tmp, "%s", line);
	}
	fclose(tmp);
	if(rv == 0 || fowner > 0 || fgroup > 0) {
		sprintf(line, "setfacl --restore=%s", tmpname);
		if(execExternal(line) != 0)
			rv = -1;
	}
	unlink(tmpname);
	fclose(f);
	return rv;
}

const char *restoreDirs(const char *path, int *emul, bool withLast) {
	char buf[PATH_MAX], cmd[PATH_MAX], *ndir, *p, *pstart, *destDir = NULL, ps;
	const char *rootDir = getRestorePath();
	strcpy(buf, path);
	ndir = (withLast) ? buf : dirname(buf);
	// skip diskID
	ps = getPathSeparator();
	p = strchr(ndir, ps);
	*emul = 0;
	if(p) {	// file at disk root directory?
		p++;
		sprintf(cmd, "%s%s", rootDir, p);
		pstart = strchr(cmd, ps);
		if(access(pstart, F_OK) != 0) {
			path4shell(cmd, sizeof(cmd));
			char shellPath[strlen(cmd) + 1];
			strcpy(shellPath, cmd);
			strcpy(cmd, "umask 077; mkdir -p %s");
			strncat(cmd, shellPath, sizeof(cmd) - strlen(cmd));
			if(execExternal(cmd) != 0)
				return NULL;
			// Restore ACL for full path
			pstart = p;
			int k = 0;
			if(rootDir == config.restoreDest) {
				strcpy(cmd, rootDir);
				destDir = cmd;
				k = strlen(cmd);
			}
			while(p) {
				if(destDir)
					strcpy(cmd + k, pstart);
				if(config.acl && restoreACL(ndir, destDir) < 0)
					*emul = -1;
				p = strrchr(pstart, getPathSeparator());
				if(p)
					*p = 0;
			};
		}
	}
	return rootDir;
}

int getTempDirPermissions() {
	return 01777;	// everyone can read/write + sticky bit
}


#include <sys/param.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <paths.h>

static struct pid {
	struct pid *next;
	FILE *fp;
	pid_t pid;
} *pidlist;

extern char **environ;
FILE *suid_popen(const char *program, const char *type)
{
	FILE *iop;
	int pdes[2];
	pid_t pid;
//	const char *argp[] = {"sh", "-c", NULL, NULL};
	if ((*type != 'r' && *type != 'w') || type[1] != '\0') {
		errno = EINVAL;
		return (NULL);
	}
	struct pid * volatile cur = (struct pid *) malloc(sizeof(struct pid));
	if (cur == NULL)
		return (NULL);
	if (pipe(pdes) < 0) {
		free(cur);
		return (NULL);
	}
	switch (pid = fork()) {
	case -1:			/* Error. */
		(void)close(pdes[0]);
		(void)close(pdes[1]);
		free(cur);
		return (NULL);
		/* NOTREACHED */
	case 0:				/* Child. */
	    {
		struct pid *pcur;
		/*
		 * We fork()'d, we got our own copy of the list, no
		 * contention.
		 */
		for (pcur = pidlist; pcur; pcur = pcur->next)
			close(fileno(pcur->fp));
		if (*type == 'r') {
			(void) close(pdes[0]);
			if (pdes[1] != STDOUT_FILENO) {
				(void)dup2(pdes[1], STDOUT_FILENO);
				(void)close(pdes[1]);
			}
		} else {
			(void)close(pdes[1]);
			if (pdes[0] != STDIN_FILENO) {
				(void)dup2(pdes[0], STDIN_FILENO);
				(void)close(pdes[0]);
			}
		}
		uid_t uid, euid, sid;
		getresuid(&uid, &euid, &sid);
		int rv = 0;
		if(uid != euid) {
			sid = getegid();
			rv = setregid(sid, sid);
			rv += setreuid(euid, euid);
		}
		execle("/bin/bash", "bash", "-c", program, (char *) NULL, environ);
		_exit(127);
		/* NOTREACHED */
	    }
	}
	/* Parent; assume fdopen can't fail. */
	if (*type == 'r') {
		iop = fdopen(pdes[0], type);
		(void)close(pdes[1]);
	} else {
		iop = fdopen(pdes[1], type);
		(void)close(pdes[0]);
	}
	/* Link into list of file descriptors. */
	cur->fp = iop;
	cur->pid =  pid;
	cur->next = pidlist;
	pidlist = cur;
	return (iop);
}
/*
 * pclose --
 *	Pclose returns -1 if stream is not associated with a `popened' command,
 *	if already `pclosed', or waitpid returns an error.
 */
int suid_pclose(FILE *iop)
{
	struct pid *cur, *last;
	int pstat;
	pid_t pid;
	/* Find the appropriate file pointer. */
	for (last = NULL, cur = pidlist; cur; last = cur, cur = cur->next)
		if (cur->fp == iop)
			break;
	if (cur == NULL)
		return (-1);
	(void)fclose(iop);
	do {
		pid = waitpid(cur->pid, &pstat, 0);
	} while (pid == -1 && errno == EINTR);
	/* Remove the entry from the linked list. */
	if (last == NULL)
		pidlist = cur->next;
	else
		last->next = cur->next;
	free(cur);
	return (pid == -1 ? -1 : pstat);
}

//int getHardLinks(const char *path, const char *result) {
//	char cmd[PATH_MAX + 128];
//	sprintf(cmd, "find %s -mount -type f -links +1 -printf '%%i %%p\n' | sort >%s", path, result);
//	return execExternal(cmd);
//}
//

//int saveHardLinks(int diskId, FILE *symFile) {
//	char cmd[2 * PATH_MAX + 128];
//	DISK *disk = config.diskDesc + diskId;
//	SQL_EXEC("DROP TABLE IF EXISTS htmp");
//	sprintf(cmd,
//		"CREATE TEMP TABLE htmp AS SELECT inode, count(*) as counts FROM newfiles WHERE disk = 'D%d' GROUP BY inode HAVING count(*) > 1",
//		diskId + 1);
//	SQL_EXEC(cmd);
//	sprintf(cmd, "SELECT inode FROM htmp");
////	SQL_COMMAND hlink = {.cmd = };
//	FILE *tmp = openTempFile("hlinks-XXXXXX");
//	SQL(&insLink, prepare);
//	{	// temporary local vatiables
//		char wrk[PATH_MAX], tmpname[PATH_MAX];
//		sprintf(wrk, "DELETE FROM links WHERE ftype = 'h' AND link_name LIKE 'D%d%%'", diskId + 1);
//		SQL_EXEC(wrk);
//		getFilePath(fileno(tmp), tmpname);
//		strcpy(wrk, disk->path);
//		path4shell(wrk, sizeof(wrk));
//		sprintf(cmd, "find %s -mount -type f -links +1 -printf '%%i %%p\n' | sort >%s", wrk, tmpname);
//		if(execExternal(cmd) != 0) {
//			fclose(tmp);
//			return 0;
//		}
//		unlink(tmpname);
//	}
//	while(fgets(cmd, sizeof(cmd), tmp) != NULL) {
//		removeLastNewline(cmd);
//		saveSpecialFile(cmd, symFile, 'h');
//	}
//	fclose(tmp);
//	SQL(&insLink, finalize);
//	return 0;
//}
//
