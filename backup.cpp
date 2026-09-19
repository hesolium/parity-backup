#include <pthread.h>
#include <dirent.h>
#include <sys/statvfs.h>
#include <fnmatch.h>
#include <printf.h>
#include "utils.h"
#include "os.h"
#include "rapidhash.h"
#include "logger.h"
#include "backup.h"
#include "asyncio.h"
#include "argparse.h"

using namespace std;

static_assert(sizeof(char *) == sizeof(int64_t), "Only for 64-bit platform");
static const char		*defaultDB = "parityBackup.db";
static const char		*defaultConf = "parityBackup.conf";
static const char		*HEADER_EXT = ".header";
static const char 		*UNRECOVER_FILE = "Unrecoverable.lst";
static const char 		*CHANGED_FILE = "Changed.lst";
static const char   	*UNRESTORED = "Unrestored.lst";
static const char   	*UNSAVED = "Unsaved-slices.lst";
#define WILDCARDS 		"?*"
#define DATA_BUF_SIZE	(8 * MEGA)
#define STAT_MASK 		(STATX_BASIC_STATS | STATX_BTIME)
#define MIN_SLICE_SIZE	208
#define	QUOTAS			"'\"`"
#define VERIFYDB_DELTA	(8 * 3600)	// every 8 hour
#define HEADER_TOKENS_MAX	5

enum { HEADER_INIT = 1, HEADER_WRITE = 2, HEADER_DUMP = 3};

CONFIG config = { // @suppress("Invalid arguments")
	.percentTolerance = DEFAULT_TOLERANCE, .maxThreads = -1, .replace = -1, .acl = true, .restoreOwner = -1, .owner = -1
};
TinyQueue jobFilter(MAX_JOBS);

static FILE *verboseFile = NULL;
bool showStatusBar;
static TCA globalTCA = {0};

TCA *getTCA() {
	return &globalTCA;
}

FILE *getVerbose() {
	return verboseFile;
}

bool cancelRequest(SLICE *cs) {
	if(globalTCA.operationCanceled) {
		if(cs)
			cs->genCanceled = true;
		if(globalTCA.queue)
			globalTCA.queue->setState(JobQueue::QUEUE_ABORT);
	}
	return globalTCA.operationCanceled;
}

bool pathMatchPattern(const char *path, int owner) {
	if(config.patternsCount == 0 && config.regex[0] == NULL && owner < 0)
		return true;
	int i = 0, st;
	bool rv = true;
	if(config.patternsCount) {
		int flags = FNM_PATHNAME | FNM_PERIOD;
		char ps = getPathSeparator();
		const char *pattern, *p;
		while((pattern = config.patterns[i])) {
			p = (pattern[0] == ps) ? strchr(path, ps) : path;	// pattern start with path separator. Skip disk ID in path
			if(!findAnyChar(pattern, WILDCARDS)) {
				if(isPrefix(pattern, p))
					goto check_owner;
			}
			else if(fnmatch(pattern, p, flags) == 0)
				goto check_owner;
			i++;
		}
	}
	for(i = 0; config.regex[i]; i++) {
		regex_t *r = config.regex[i];
		st = regexec(r, path, 0, NULL, 0);
		if(st == 0)
			goto check_owner;

	}
	return false;
check_owner:
	// found pattern
	rv = true;
	if(owner >= 0) {
		char lbuf[PATH_MAX];
		strcpy(lbuf, path);
		int ow = getFileOwner(lbuf);
		if(ow != owner)
			return false;
	}
	return rv;
}

SLICE_DISK *getParityDisk(SLICE *cs) {
	return cs->disks + config.disks;
}

bool isParity(int diskIdx) {
	return diskIdx == config.disks;
}

ulong time2nano(struct timespec *ts) {
	return ts->tv_sec * 1000000000 + ts->tv_nsec;
}

bool diskIsMounted(int diskIdx) {
	if(diskIdx < 0)
		return config.diskDesc[PARITY_DISK - 1].node_size != 0;
	return config.diskDesc[diskIdx].node_size != 0;
}

int getDiskStat(DISK *disk, const char *path) {
	struct statvfs stfs = {0};
	bzero(disk, sizeof(DISK));
	const char *mp;
	int rv = -1;
	mp = disk->mountDir = getMountDir(path);
	if(!isUUID(path)) {
		char wrk[strlen(path) + 4];
		strcpy(wrk, path);
		addPathSeparator(wrk);
		mp = strdup(wrk);
	} else if(mp)
		mp = strdup(mp);
	if(mp) {
		disk->path = mp;
		rv = statvfs(mp, &stfs);
		if(rv == 0)	{ // mounted
			disk->node_size = stfs.f_bsize;
			disk->disk_size = stfs.f_blocks * stfs.f_frsize;
			disk->free_size = stfs.f_bavail * stfs.f_bsize;
			struct stat fst = {0};
			rv = stat(mp, &fst);
			if(rv == 0)
				disk->f_sid = fst.st_dev;
		}
	} else
		disk->path = strdup(path);
	return rv;
}

static int duplicatedDevice(int didx) {
	ulong fsid = config.diskDesc[didx].f_sid;
	for(int i = 0; i < PARITY_DISK; i++) {
		if(i != didx && config.diskDesc[i].f_sid == fsid)
			return i;
	}
	return -1;
}

static void listDisks() {
	bool checkDifferent = config.cmd == SYNC && !config.allowSameDevices;
	bool mustBeMounted = (config.cmd == SYNC || config.cmd == RESTORE);
	int omitDisk = -1;
	log_debug("\nDisks specification\n");
	char msg[4] = "  ",	size[64], size2[64];
	const char *fmsg = NULL;
	int nd = PARITY_DISK, k;
	if(config.restoreDest) {
		omitDisk = getDiskIndex(config.restoreDiskId);
		nd++;
	}
	log_debug("Id          Size    Free space    Mount dir//Path\n");
	log_debug("----  ----------    ----------    ---------------\n");
	for(int i = 0; i < nd; i++) {
		DISK *dsk = config.diskDesc + i;
		if(dsk->node_size) {	// mounted
			msg[0] = ' ';
			k = duplicatedDevice(i);
			if(k >= 0 && checkDifferent)
				fmsg = "All disk must be on different physical devices.";
			if(k >= 0 && k < i) {
				strcpy(size2, "         ");
				sprintf(size, "- as D%d -", k + 1);
			} else {
				formatSize(dsk->disk_size, size, false);
				formatSize(dsk->free_size, size2, false);
			}
		} else {
			msg[0] = '!';
			strcpy(size, "         ");
			strcpy(size2, size);
			if(mustBeMounted && i != omitDisk)
				fmsg = "Disk must be mounted!";
		}
		k = strlen(dsk->mountDir);
		log_debug("%-5s %11.11s   %11.11s %s%s/%s\n", dsk->id, size, size2, msg, dsk->mountDir, dsk->path + k);
		if(fmsg)
			PERROR(LOG_FATAL, fmsg);
	}
	log_debug("\n");
}

const char *getRestorePath() {
	const char *dest = config.restoreDest;
	if(config.restoreDest == NULL)
		dest = config.diskDesc[getDiskIndex(config.restoreDiskId)].path;
	return dest;
}

static int clearWorkDir(const char *dpath = NULL) {
	char fpath[PATH_MAX];
	if(dpath == NULL)
		dpath = (config.cmd == RESTORE) ? getRestorePath() : config.workDir;
	sprintf(fpath, "%s%s", dpath, TEMP_SUBDIR);
	int rv = 0;
	if(cleanDir(fpath) != 0) {
		int omask = umask(0);
		rv = mkdir(fpath, getTempDirPermissions());	// everyone can read/write + sticky bit
		if(rv != 0)
			PERROR(LOG_FATAL, "Can't create temporary directory: %s", fpath);
		umask(omask);
	}
	return rv;
}

void storeHash(SLICE *cs, int diskIdx, sqint seq, const char *hash) {
	SLICE_DISK *sd = cs->disks + diskIdx;
	SLICE_FILE *cf = &sd->file;
	if(cf->fileSize == 0)
		return;
	if(cs->hashCount >= cs->hashSize)
		log_fatal("Hash array too small: %d", cs->hashCount);
	HASH_POS *hp = cs->hashArray + cs->hashCount++;
	if(hash == NULL)
		getBlockHash(cf->hashState, hp->hash);
	else
		COPY_FLD(hp->hash, hash);
	if(seq == 0)
		seq = sd->seq;
	hp->dseq = seq;
	hp->disk = diskIdx;
}

bool findHash4seq(SLICE *cs, int diskIdx, sqint seq, char *hbuf) {
	hbuf[0] = 0;
	for(uint i = 0; i < cs->filesCount; i++) {
		HASH_POS *hp = cs->hashArray + i;
		if(hp->dseq == seq && hp->disk == diskIdx) {
			strcpy(hbuf, cs->hashArray[i].hash);
			return true;
		}
	}
	return false;
}

#include <aio.h>

int cancelGen(SLICE *cs, const char *fmt, ...) {
	int rv = 0;
	char msg[PATH_MAX] = "", *start = msg;
	if(cs) {
		if(cs->genCanceled)
			return 0;
		cs->genCanceled = true;
		cs->outOfSpace = (errno == EDQUOT || errno == EFBIG || errno == ENOSPC);
		if(cs->outOfSpace)
			rv = -1;
	}
	if(fmt) {
		va_list args;
		va_start(args, fmt);
		vsnprintf(msg, sizeof(msg), fmt, args);
		va_end(args);
		if(fmt[0] == '#')
			start = msg + 1;
		else
			log_info(msg);
	}
	if(cs && cs->raport)
		fprintf(cs->raport, "%s\n--> %s\n\n", cs->savePath, start);
	return rv;
}

void printSliceState(SLICE *cs, const char *msg, int diskIdx, int dumpSize) {
	if(getVerbose()) {
		int start = diskIdx, stop = diskIdx + 1, blen;
		LOCK_VERBOSE();
		VERBOSE("\n%s\n", msg);
		if(diskIdx < 0) {
			start = 0;
			stop = PARITY_DISK;
			VERBOSE("Slice : %s   Size: %s   Header: %d   writeStart: %ld\n",
				cs->sliceId, formatFileSize(cs->sliceSize), cs->headerSize, cs->writeStart);
		}
		char marker, disk, path[PATH_MAX], *rpath;
		for(int i = start; i < stop; i++) {
			SLICE_DISK *sd = cs->disks + i;
			SLICE_FILE *sf = &sd->file;
			marker = ' ';
			if(i == cs->disk2write)
				marker = '>';
			disk = (isParity(i)) ? 'P' : '0' + i + 1;
			path[0] = 0;
			rpath = path;
			if(sd->fd > 0) {
				getFilePath(sd->fd, path);
				if(isParity(i)) {
					rpath += strlen(PARITY_DIR);
					*--rpath = '/';
					*--rpath = disk;
				} else if(i != cs->disk2write) {
					rpath += strlen(config.diskDesc[i].path);
					*--rpath = '/';
					*--rpath = disk;
					*--rpath = 'D';
				}
			}
			VERBOSE("Disk: %c%c   seq: %ld   fileSize: %ld   parityStart: %ld   fileOffset: %ld   inBuffer: %d   path: %s\n",
				disk, marker, sd->seq, sf->bytes2transfer, sf->parityStart, sd->fileOffset, sd->datalen, rpath);
			if(sf->bytes2transfer < 0)
				log_fatal("Incorrect bytes2transfer field");
			if(cs->aio_cb) {
				aiocb *cb =  ((aiocb *) cs->aio_cb) + i;
				if(cb->aio_nbytes > 0) {
					VERBOSE("         AIOCB: fileOffset: %ld  opLength: %ld  bufOffset: %ld\n",
						cb->aio_offset, cb->aio_nbytes, ((char *) cb->aio_buf) - sd->dataBuf);
				}
			}
			if(dumpSize > 0) {
				blen = MIN(sd->datalen, dumpSize);
				hexDump(verboseFile, sd->dataBuf, blen, "         ");
			}
		}
		UNLOCK_VERBOSE();
	}
}

void lockVerbose() {
	if(verboseFile)
		flockfile(verboseFile);
}

void unlockVerbose() {
	if(verboseFile)
		funlockfile(verboseFile);
}

int verbose(const char *fmt, ...) {
	if(verboseFile) {
		va_list args;
		va_start(args, fmt);
		int rv = vfprintf(verboseFile, fmt, args);
		va_end(args);
		return rv;
	}
	return 0;
}

char *removeComments(char *line) {
	char *p = strchr(line, '#');
	if(p)
		*p = 0;
	p = ltrim(line);
	return p;
}

int getStatFD(int fd, struct stat *statbuf) {
	bzero(statbuf, sizeof(struct stat));
	return fstat(fd, statbuf);
}

void copyConfig(bool confOnly, bool dryBackup) {
	// copy files: config, SQLite database, list of files on disk, ACL
	if(strcmp(config.workDir, config.configDir)) {
		char src[2 * PATH_MAX];
		const char *dst = (dryBackup) ? src : config.configDir;
		if(!confOnly) {
			const char *db = defaultDB, *dbDir = config.workDir;
			sprintf(src, " %s%s %s*.files %s*.acl %s*.lnk",	dbDir, db, config.workDir, config.workDir, config.workDir);
			log_info("Copy control files to reserve %s directory ...", dst);
		}
		if(dryBackup) {
			if(!confOnly)
				copyFiles(dst, src);
			sprintf(src, "%s%s", config.workDir, defaultConf);
			copyFiles(dst, src, RUMASK);
		} else {
			cleanDir(config.configDir);
			if(!confOnly)
				copyFiles(src, dst);
			sprintf(src, "%s%s", config.workDir, defaultConf);
			copyFiles(src, dst, RUMASK);
		}
	}
}

void exitProg(int rv) {
//	signal(SIGINT, SIG_IGN);
	if(rv == 0)
		SQL_EXEC("UPDATE watchdog SET prog_start = 0");
	closeDB();
	const char *mess = (rv < 0) ? "failed" : "finished";
	if(rv == 0)
		copyConfig();
	log_info("=============== Backup %s\n", mess);
	exit(rv);
}

void intHandler(int signum) {
	if(signum == SIGFPE) {
		signal(SIGFPE, SIG_IGN);
		log_fatal("Arithmetic error. Core dumped\n");
	}
	if(signum == SIGINT) {
		signal(SIGINT, SIG_IGN);
		TCA *tca = getTCA();
		if(tca->start == 0)	// TCA not initialised
			exitProg(0);
		log_info("*** Program terminated by SIGINT.");
		tca->operationCanceled = true;
		log_info("*** Long run process active. Wait for cleanup");
	}
}

int prepareConfigLine(char *buf, int bufSize, FILE *f) {
	char *p;
	while(1) {
		p = fgets(buf, bufSize, f);
		if(p == NULL)
			return 0;
		char *ln = removeComments(buf);
		if(ln[0] == 0)
			continue;
		if(ln != buf)
			memmove(buf, ln, bufSize - (ln - buf));
		char *val = strchr(buf, '=');
		if(val == NULL)
			return -1;
		*val++ = 0;
		val = ltrim(val);
		rtrim(val);
		rtrim(buf);
		return val - buf;
	}
}

int readConfigFile() {
	FILE *f = NULL;
	char line[PATH_MAX];
	const char *parityDir = config.diskDesc[MAX_DISKS - 1].path;
	if(config.configDir == NULL) {
		normPath(DEFAULT_CONFIG_DIR, line);
		if(fileInfo("", line) < 0) {
//			int mask = umask(0);
			if(mkdir(line, 0777) != 0)
				PERROR(LOG_FATAL, "Can't create default offline %s directory", DEFAULT_CONFIG_DIR);
//			umask(mask);
		}
		config.configDir = strdup(line);
	}
	if(config.configFile) {
		f = fopen(config.configFile, "r");
		if(f == NULL)
			PERROR(LOG_FATAL, "Configuration file %s not found", config.configFile);
	}
	if(parityDir) {
		struct statvfs stfs = {0};
		if(statvfs(parityDir, &stfs) == 0) { // Parity backup disk specified and mounted
			sprintf(line, "%s%s", parityDir, DEFAULT_BACKUP_SUBDIR);
			if(fileInfo(parityDir, DEFAULT_BACKUP_SUBDIR) < 0) {
				if(mkdir(line, 0777) != 0)
					PERROR(LOG_FATAL, "Can't create default backup %s subdirectory", line);
			}
			free((void *)parityDir);
			// backup subdir on backup disk
			config.workDir = parityDir = config.diskDesc[MAX_DISKS - 1].path = strdup(line);
			if(f == NULL) {
				f = openAt(defaultConf, "r", config.workDir);
				if(f == NULL) {
					if(config.dry && config.cmd == SYNC) {
						const char *cdir = (config.configDir) ? config.configDir : DEFAULT_CONFIG_DIR;
						f = openAt(defaultConf, "r", cdir);
						if(f) {
							log_info("Dry run. Switch to temporary backup disk");
							copyConfig(false, true);
						}
					}
				}
			}
			if(f == NULL)
				PERROR(LOG_FATAL, "Configuration file %s not exist on backup disk %s", defaultConf, parityDir);
		} else
			PERROR(LOG_FATAL, "Backup disk %s is specified in program args but not mounted. Mount or remove from args", parityDir);
	}
	if(f == NULL) {
		if(config.workDir == NULL)
			config.workDir = config.configDir;
		f = openAt(defaultConf, "r", config.workDir);
		if(f == NULL)
			PERROR(LOG_FATAL, "Config file %s not found", defaultConf);
	}
	log_notime("Read config data from %s", getFilePath(fileno(f), line));	// @suppress("Invalid arguments")
	sqint fs = fileInfo(f);
	if(fs > PATH_MAX)
		fs = PATH_MAX;
	char excludeDirs[fs] = {0}, *exDir = excludeDirs, excludeNames[fs] = {0}, *exName = excludeNames;
	char excludeRegex[fs] = {0}, *exRegex = excludeRegex;
	int rv = -1, st;
	DISK wrk = {0};
	while ((st = prepareConfigLine(line, sizeof(line), f)) > 0) {
		char *val = line + st;
		if(tolower(line[0]) == 'b') {	// backup directory
			st = getDiskStat(&wrk, val);
			if(wrk.path) {
				sprintf(val, "%s%s", wrk.path, DEFAULT_BACKUP_SUBDIR);
				free((void *)wrk.path);
				wrk.path = strdup(val);
			}
			if(parityDir == NULL) {
				config.diskDesc[MAX_DISKS - 1] = wrk;
				parityDir = wrk.path;
				if(st == 0 && fileInfo("", wrk.path) < 0) {
					if(mkdir(wrk.path, 0777) != 0)
						PERROR(LOG_FATAL, "Can't create default backup %s subdirectory", line);
				}
			} else {
				if(wrk.path) {
					if(strcmp(parityDir, wrk.path)) {
						if(config.dry && config.cmd == SYNC)
							PERROR(LOG_WARN, "Dry run. Alternate backup disk [%s] accepted.", parityDir);
						else
							PERROR(LOG_FATAL, "Backup disk in args and in config file differents!. [%s] != [%s]", wrk.path, parityDir);
					}
					free((void *)wrk.path);
				} else
					PERROR(LOG_FATAL, "Backup disk in args and in config file differents!. [%s] != [%s]", val, parityDir);
			}
		} else if(tolower(line[0]) == 'd') {	// data directory
			if(config.disks >= MAX_DISKS)
				PERROR(LOG_FATAL, "Too many data disks in config file!");
			line[0] = toupper(line[0]);
			int did = getDiskIndex(line);
			if(did >= MAX_DISKS || did < 0 || config.diskDesc[did].path)
				PERROR(LOG_FATAL, "Incorrect disk IDX in config file!");
			config.disks++;
			getDiskStat(&wrk, val);
			char lb[16];
			sprintf(lb, "D%d", did + 1);
			memcpy(wrk.id, lb, sizeof(wrk.id));
			config.diskDesc[did] = wrk;
		}
//		else if(tolower(line[0]) == 'f')
//			strcat(filter, val);
//		else if(strcmp(line, "path-exclude-option") == 0)
//			config.excludePathPhrase = strdup(val);
//		else if(strcmp(line, "regex-exclude-option") == 0)
//			config.excludeRegexPhrase = strdup(val);
//		else if(strcmp(line, "name-exclude-option") == 0)
//			config.excludeNamePhrase = strdup(val);
		else if(strcmp(line, "exclude-dir") == 0) {
			st = strlen(val);
			if(st > 0) {	// plex
				st++;
				memcpy(exDir, val, st);
				exDir += st;
			}
		}
		else if(strcmp(line, "exclude-regex") == 0) {
			st = strlen(val);
			if(st > 0) {
				st++;
				memcpy(exRegex, val, st);
				exRegex += st;
			}
		}
		else if(strcmp(line, "exclude-names") == 0) {
			st = strlen(val);
			if(st > 0) {
				st++;
				memcpy(exName, val, st);
				exName += st;
			}
		}
	}
	fclose(f);
	if(st < 0)
		PERROR(LOG_FATAL, "Illegal command in config file: %s", line);
	strcpy(line, DEFAULT_BACKUP_SUBDIR);
	char *p = strrchr(line, getPathSeparator());
	if(p)
		*p = 0;
	st = strlen(line);
	if(st > 0) {	// plex
		st++;
		memcpy(exDir, line, st);
		exDir += st;
	}
	rv = 0;
	if(config.diskDesc[MAX_DISKS - 1].path == NULL)
		PERROR(LOG_FATAL, "Backup destination disk must be specified");
	// Set backup disk info in last cell
	config.diskDesc[config.disks] = config.diskDesc[MAX_DISKS - 1];
	// Check data disk
	for(int i = 0; i < config.disks + 1; i++) {
		DISK *dsk = config.diskDesc + i;
		if(dsk->path == NULL)
			PERROR(LOG_FATAL, "Incorrect disk IDX in config file. IDX must be from 1 .. nDiscs!");
	}
	if(diskIsMounted())	// backup disk is mounted
		config.workDir = parityDir;
//	if(strlen(filter) == 0)
//		PERROR(LOG_FATAL, "Filter program not specified in config.file");
//	config.filter = strdup(filter);
	st = exDir - excludeDirs;
	if(st > 0) {
		st += 8;
		config.excludePaths = (char *) malloc(st);
		memcpy(config.excludePaths, excludeDirs, st);
	}
	st = exName - excludeNames;
	if(st > 0) {
		st += 8;
		config.excludeNames = (char *) malloc(st);
		memcpy(config.excludeNames, excludeNames, st);
	}
	st = exRegex - excludeRegex;
	if(st > 0) {
		st += 8;
		config.excludeRegex = (char *) malloc(st);
		memcpy(config.excludeRegex, excludeRegex, st);
	}
	if(config.configFile)
		copyFiles(config.configFile, config.workDir, RUMASK);
	else if(fileInfo(config.workDir, defaultConf) <= 0) {
		sprintf(line, "%s%s", config.configDir, defaultConf);
		copyFiles(line, config.workDir, RUMASK);
	}
	return rv;
}

static const char *const usages[] = {
    "Pbackup [options] [[--] command [command args]]",
    "Pbackup [options] command [command args]",
    NULL,
};

static int savePatterns(int argc, const char** argv, int startIdx, const char *prefix) {
	const char *patterns[MAX_PATTERN_SIZE] = {0}, *arg;
	char pattern[PATH_MAX] = "", ps = getPathSeparator();
	int n = 0, k = 0, regN = 0, rflags = REG_EXTENDED | REG_NOSUB, reg, quot = 0;
	if(prefix) {
		COPY_FLD(pattern, prefix);
		k = strlen(prefix);
	}
	if(argc >= MAX_PATTERN_SIZE)
		PERROR(LOG_FATAL, "Too much file patterns");
	for(int i = startIdx; i < argc; i++) {
		arg = argv[i];
		reg = (arg[0] == 'r') ? 1 : 0;
		if(strchr(QUOTAS, arg[reg])) {
			quot = 1;
			int len = strlen(arg + reg);
			if(arg[reg] != arg[reg + len - 1])
				PERROR(LOG_FATAL, "Argument quotation not match: %s", arg);
			arg += reg + 1;
		} else
			reg = 0;
		if(*arg == ps)
			arg++;	// omit first '/'
		strcpy(pattern + k, arg);
		if(quot)
			pattern[strlen(pattern) - 1] = 0;
//		p = *arg;
//		if(strchr(WILDCARDS, p) == NULL && p != 'D' && p != '[' && p != 'r')
//			PERROR(LOG_FATAL, "Inactive filter pattern '%s'. Pattern must start with wildcar: '%s%c', disk Id (D<n>), path separator '%c' or regex marker 'r'",
//				arg, WILDCARDS, '[', ps);
		if(reg) {
			// regular exp r....
			config.regex[regN] = (regex_t *) calloc(1, sizeof(regex_t));
			if(regcomp(config.regex[regN], pattern, rflags) < 0)
				PERROR(LOG_FATAL, "Can't compile regular expression: %s", pattern);
			regN++;
			continue;
		}
		patterns[n++] = strdup(pattern);
	}
	if(n > 0) {
		config.patternsCount = n;
		config.patterns = (const char **) malloc(n * sizeof(char *));
		memcpy(config.patterns, patterns, n * sizeof(char *));
	}
	return n;
}
// -------------------------------------------------------------
int newFileFlags(int fd, int newFlags, int set) {
	if(newFlags != 0) {
		int oldflags = fcntl(fd, F_GETFL, 0);
		if (oldflags < 0)
			return oldflags;
		if(set)
			oldflags |= newFlags;
		else
			oldflags &= ~newFlags;
		return fcntl (fd, F_SETFL, oldflags);
	}
	return 0;
}

//int argCallback(struct argparse *self, const struct argparse_option *option) {
////	static int n = 0;
//	if(option->short_name == 'o') {
//        const char *ow = NULL;
//        if(self->argc > 1 && self->argv[1][0] != '-') {
//        	// consume
//            self->argc--;
//        	ow = *++self->argv;
//        }
//        if(ow) {
//    		config.owner = findUser(ow);
//    		if(config.owner < 0)
//    			PERROR(LOG_FATAL, "Owner ID specified (%s) not exist.", ow);
//        } else {
//        	config.owner = getuid();
//        }
//	}
//	return 0;
//}

const char *commNames[] = {	"plan", "sync", "restore", "show"};
static_assert((sizeof(commNames) / sizeof(char *)) == (SHOW - PLAN) + 1, "Inconsistent command table size with enum elements");

static void getCommand(const char *cname) {
	for(uint i = 0; i < sizeof(commNames) / sizeof(char *); i++) {
		if(strcmp(cname, commNames[i]) == 0) {
			config.cmd = (COMMANDS) (((int) PLAN) + i);
			return;
		}
	}
}

const char *comms =
	"\nCommands:"
	"\nsync - create/update backup drive\n"
	"\nrestore d<n>[=dest] [patterns ...] - restore disk/dir/file from backup\n"
	"\ncheck [patterns ...] - lists files that may be unrecoverable due to changes to the original files.\n"
	"\nshow c[hanged]/u[nrecover] [patterns ...] - show some info on DB\n"
	"\ncont - continues SYNC operation after a interrupted run.\n"
	"\n"
	"\n* [options specific for restore command]"
	"\n  flat      - restore files without full path"
	"\n  repl[ace] - replace newer files with older from archive"
	"\n  noacl     - not restore file ownership and permission"
	"\n  zero      - like repl[ace] but zero length files are replaced too"
	"\n  owner=id  - restore only files for owner id (numeric or symbolic)\n"
	"\n* [pattern] is expression with optional wildcard as described in 'fnmatch' standard Linux function"
	" or regular expression if started by prefix r' e.g. r'.*txt.*'"
	;

int initProgram(int argc, const char** argv) {
	char *argsLine = getArgsLine(argc, argv);
	signal(SIGFPE, &intHandler);
	signal(SIGINT, &intHandler);
	loggerExitFunc(exitProg);
//	char *owner = NULL;
	struct argparse_option options[] = {
		OPT_GROUP("Basic options"),
        OPT_STRING('b', "backup", &config.diskDesc[MAX_DISKS - 1].path, "Full path to backup disk directory", NULL, 0, 0),
        OPT_INTEGER('t', "threads", &config.maxThreads, "Number of sync/restore threads", NULL, 0, 0),
        OPT_STRING('c', "config", &config.configDir, "Reserve config directory. Default /parity-backup/", NULL, 0, 0),
        OPT_STRING('l', "log", &config.log, "Log file", NULL, 0, 0),
        OPT_INTEGER('m', "max-files", &config.maxFilesInSlice, "Max files in slice for one disk. Default: 1000", NULL, 0, 0),
        OPT_BOOLEAN('h', "help", &config.help, "Show program summary and exit", NULL, 0, 0),
        OPT_INTEGER(0, "buffer-size", &config.fileBufferSize, "Buffer size for disk IO (in MB)", NULL, 0, 0),
//		OPT_BOOLEAN(0, "rebuild", &config.rebuild, "Rebuild Sqlite DB from saved backup slices", NULL, 0, 0),
        OPT_GROUP("Option specific to restore command"),
        OPT_STRING('r', "roption", &config.options, "Options separated by commas: noacl,flat,repl[ace],zero,owner=id", NULL, 0, 0),
        OPT_GROUP("Helper options for development and tests"),
        OPT_BOOLEAN(0, "verbose", &config.verbose, "Additional debug messages in log file", NULL, 0, 0),
        OPT_INTEGER('s', "slices", &config.maxSlices, "Max slices to generate", NULL, 0, 0),
		OPT_BOOLEAN(0, "same-disks", &config.allowSameDevices, "Allow 'disks' reside on the one physical device", NULL, 0, 0),
		OPT_STRING(0, "sqliteMode", &config.sqliteMode, "[r]ebuild or [f]orce", NULL, 0, 0),
        OPT_BOOLEAN('v', "verify", &config.verify, "Force verify slice files with DB records", NULL, 0, 0),
        OPT_STRING(0, "dry", &config.dryOption, "Value: h[ead]/r[ead]/[c]ompare. Sync/restore write only skeleton data on target disk or nothing.",
       		NULL, 0, 0),
		OPT_END()
	};
    struct argparse argparse;
    argparse_init(&argparse, options, usages, 0);
    argparse_describe(&argparse, "\nBackup program for array of disks. Parity archive is saved on separate drive.\nVersion 0.99", comms);
    argc = argparse_parse(&argparse, argc, argv);
    if(config.help) {
    	argparse_usage(&argparse);
    	exit(0);
    }
    if(config.log) {
    	int exist = (access(config.log, F_OK) == 0);
		const char *mode = (exist) ? "r+" : "w";
		int omask = umask(RUMASK);
		FILE *logFile = fopen(config.log, mode);
		umask(omask);
		if(logFile) {
			setvbuf(logFile, NULL, _IONBF, 0);
			if(exist)
				fseek(logFile, 0, SEEK_END);
			log_add_fp(logFile, LOG_DEBUG);
			if(config.verbose)
				verboseFile = logFile;
		} else
			fprintf(stderr, "Can't open log file: %s\n", config.log);
    }
	if(config.verbose && verboseFile == NULL)
		verboseFile = stdout;
	int tty = isatty(fileno(stdout));
	showStatusBar = (verboseFile != stdout && verboseFile != stderr && (tty || config.log));
	if(showStatusBar && tty)
		setvbuf(stdout, NULL, _IONBF, 0);
    if(tty)
		log_add_fp(stdout, LOG_DEBUG);
	log_info("Backup start with args: %s", argsLine);
	free(argsLine);
	if(config.userId != config.euserId) {
		struct passwd *puid;
		char usr[128], eusr[128];
		puid = getpwuid(config.userId);
		if(puid)
			strcpy(usr, puid->pw_name);
		else
			sprintf(usr, "%d", config.userId);
		puid = getpwuid(config.euserId);
		if(puid)
			strcpy(eusr, puid->pw_name);
		else
			sprintf(eusr, "%d", config.euserId);
		log_info("Run in SUID mode. Real: %s    Effective: %s", usr, eusr);
	}
    if(argc == 0) {
    	PERROR(LOG_ERROR, "Expected one of the command!");
    	argparse_usage(&argparse);
    	exit(1);
    }
    if(argc > 0) {
    	getCommand(argv[0]);
		if(config.cmd == 0) {
			PERROR(LOG_ERROR, "Unknown command: %s", argv[0]);
			argparse_usage(&argparse);
			exit(1);
		}
    }
    if(config.sqliteMode == NULL)
    	config.sqliteMode = "";
    else if(config.sqliteMode[0] != 'r' && config.sqliteMode[0] != 'f')
    	PERROR(LOG_FATAL, "sqliteMode option must be [r]ebuild or [f]orce");
    DISK restoreDisk = {0};
    if(argc > 1) {
    	// pattern for restore
    	if(config.cmd == RESTORE) {
    		if(config.userId != config.euserId)
    			config.restoreOwner = config.userId;
        	char *p;
        	char pattern[PATH_MAX];
        	strcpy(pattern, argv[1]);
    		p = strchr(pattern, '=');
    		if(p) {
    			// target specified
    			addPathSeparator(p);
				config.restoreDest = strdup(p + 1);
				*p = 0;
				config.restoreDiskId = strdup(pattern);
				getDiskStat(&restoreDisk, p + 1);
				strcpy(restoreDisk.id, "RST");
    		}
			addPathSeparator(pattern);
    		int k = getDiskIndex(pattern);
    		k = sprintf(pattern, "D%d%c", k + 1, getPathSeparator());
			savePatterns(argc, argv, 2, pattern);
    	} else if(config.cmd == SHOW) {
			savePatterns(argc, argv, 2, NULL);
    	}
    	else if(config.cmd == SYNC || config.cmd == PLAN) {
    		// optional (init) config file path
    		config.configFile = argv[1];
    	}
    } else if(config.cmd == RESTORE || config.cmd == SHOW) {
    	PERROR(LOG_FATAL, "Not additional, obligatory argument for command");
    }
	if(config.options) {
		// decode option for restore
		char *token[8] = {NULL};
		int n = split(config.options, token, 8, ",");
		for(int i = 0; i < n; i++) {
			if(strcmp(token[i], "zero") == 0)
				config.replace = 0;
			if(strncmp(token[i], "repl", 4) == 0)
				config.replace = 1;
			else if(strcmp(token[i], "flat") == 0)
				config.restoreFlat = true;
			else if(strcmp(token[i], "noacl") == 0)
				config.acl = false;
			else if(strncmp(token[i], "owner", 5) == 0) {
				if(token[i][5] == '=') {
					config.restoreOwner = findUser(token[i] + 6);
					if(config.restoreOwner < 0)
						PERROR(LOG_WARN, "Specified user ID (%s) unknown!", token[i] + 6);
				} else
					config.restoreOwner = config.userId;
			}
			else {
				PERROR(LOG_FATAL, "Illegal '--option' value:", token[i]);
			}
		}
	}
	const char *parityDir = config.diskDesc[MAX_DISKS - 1].path;
	if(parityDir)
		getDiskStat(config.diskDesc + MAX_DISKS - 1, parityDir);
	if(config.dryOption) {
		if(config.dryOption[0] != 'r' && config.dryOption[0] != 'h' && config.dryOption[0] != 'c')
			PERROR(LOG_FATAL, "Illegal '--dry' option value:", config.dryOption);
		config.dry = config.dryOption[0];
	}
	readConfigFile();
	logTimeFormat("%H:%M:%S");
	strcpy(config.diskDesc[config.disks].id, "BCK");
	if(config.cmd == RESTORE) {
		if(config.restoreDest)
			config.diskDesc[PARITY_DISK] = restoreDisk;	// After backup
		int didx = getDiskIndex(config.restoreDiskId);
		if(didx < 0 || didx >= config.disks)
			PERROR(LOG_FATAL, "Bad disk ID for restore: %s", config.restoreDiskId);
		config.diskDesc[didx].path = config.restoreDest;
	}
	parityDir = config.diskDesc[MAX_DISKS - 1].path;
	if(parityDir[0] != '/' && !isUUID(parityDir)) {
		PERROR(LOG_FATAL, "Backup disk must be absolute path to directory or UUID");
		exit(1);
	}
	if(!diskIsMounted()) {
		if(config.cmd == SYNC) {
			if(config.dry != 'h')
				PERROR(LOG_FATAL, "For '%s' command mount backup disk or specify '--dry h' option", argv[0]);
			// Dry run redirect to work
			DISK *dsk = config.diskDesc + config.disks;
			free((void *)dsk->path);
			dsk->path = (char *) config.workDir;
			log_info("Dry run. Slice headers redirected to '%s' directory", config.workDir);
		}
		else if(config.sqliteMode[0] == 'r' || config.cmd == RESTORE)
			PERROR(LOG_FATAL, "For '%s' command backup disk must be mounted", argv[0]);
	}
	if(config.cmd == RESTORE && config.restoreFlat && config.restoreDest == NULL) {
		PERROR(LOG_FATAL, "Flat restore not allowed for original data disk. Specify target directory");
	}
	listDisks();
	if(config.maxFilesInSlice <= 0)
		config.maxFilesInSlice = DEFAULT_MAX_SLICE_FILES;
	if(config.fileBufferSize <= 0)
		config.fileBufferSize = DATA_BUF_SIZE;
	else if(config.fileBufferSize > 16) {
		PERROR(LOG_FATAL, "IO Buffer size for one disk too big. (Max: 16 MB)");
	}
	else
		config.fileBufferSize *= MEGA;
	if(config.maxThreads < 0)
		config.maxThreads = procCores();
	return 0;
}

int computeParity(SLICE *cs) {
	cancelRequest(cs);
	if(cs->genCanceled)
		return 0;
	int parLen = 0;
	int dw = cs->disk2write;
	SLICE_DISK *sd;
	for(int i = 0; i < PARITY_DISK; i++) {
		if(i != dw) {
			sd = cs->disks + i;
			if(sd->datalen > parLen)
				parLen = sd->datalen;
		}
	}
	// destination disk (parity or data)
	SLICE_DISK *parsd = cs->disks + dw;
	parsd->datalen = parLen;
	if(parLen > 0) {
		for(int k = 0; k < parLen; k++) {
			char parityByte = 0;
			for(int i = 0; i < PARITY_DISK; i++) {
				if(i != dw) {
					sd = cs->disks + i;
					parityByte ^= sd->dataBuf[k];
				}
			}
			parsd->dataBuf[k] = parityByte;
		}
		printSliceState(cs, "After parity", -1, 32);
		for(int i = 0; i < PARITY_DISK; i++) {
			if(i != dw) {
				sd = cs->disks + i;
				sd->datalen = 0;
				bzero(sd->dataBuf, parLen);
			}
		}
	} else {
		// All files processed
		sd = cs->disks + dw;
		if(sd->file.bytes2transfer != 0) {
			char path[PATH_MAX];
			parLen = cancelGen(cs, "Incomplete (%d bytes) target file %s.", sd->file.bytes2transfer, getFilePath(sd->fd, path));
		}
	}
	return parLen;
}

void sliceFileName(const char *sliceId, char *path, const char *prefix) {
	sprintf(path, "%s%s%s", PARITY_DIR, prefix, sliceId);
}

void freeHashArray(SLICE *cs) {
	if(cs->hashArray) {
		free(cs->hashArray);
		cs->hashArray = NULL;
	}
	cs->hashCount = cs->hashSize = 0;
}

void freeSlice(SLICE *cs) {
	freeHashArray(cs);
	for(int i = 0; i < PARITY_DISK; i++) {
		SLICE_DISK *sd = cs->disks + i;
		if(sd->dataBuf) {
			free(sd->dataBuf);
			sd->dataBuf = NULL;
			if(sd->file.hashState) {
				freeHashState(sd->file.hashState);
				sd->file.hashState = NULL;
			}
		}
	}
}

int decodeHeaderLine(char *buf, char *tok[], int maxTokens = 0) {
	removeLastNewline(buf);
	return split(buf, tok, HEADER_TOKENS_MAX, BLANKS, maxTokens);
}

char *decodeEOHline(char *line, sqint *ssize, sqint *files) {
	char *tok[HEADER_TOKENS_MAX];
	int n = decodeHeaderLine(line, tok);
	if(n != 4 || strcmp(tok[0], EOH) != 0)
		return NULL;
	// EOH line
	*ssize = atol(tok[1]);
	*files = atol(tok[2]);
	rtrim(tok[3]);
	return tok[3];
}

int loadSliceHeader(char *argPath, SLICE *cs = NULL) {
	FILE *fp = NULL;
	char *fpath = argPath;
	bool intoDB = (cs == NULL);
	if(fpath) {
		char *sep = strrchr(fpath, '.');
		if(sep && strcmp(sep, HEADER_EXT) == 0)
			return 0;
		fp = fopen(fpath, "r");
		fpath = basename(fpath);
	} else {
		fp = openAt(cs->sliceId, "r", PARITY_DIR);
		fpath = cs->sliceId;
	}
	if(fp == NULL) {
		errno = 0;
		return -1;
	}
	SQL_COMMAND dbcmd = {0};
	char state[16];
	SQL_MARKER(state);
	const void *bvars[] = {fpath};
	const void *locvars[] = {state};
	int st = SQL_EXEC("SELECT state FROM slhead WHERE sid = ?", NULL, bvars, locvars);
	if(intoDB) {
		if(st > 0) {
			if(state[0] != 'D')
				SQL_EXEC("UPDATE slhead SET state = 'D' WHERE sid = ?", NULL, bvars);
			fclose(fp);
			return 0;
		}
		dbcmd.cmd = "INSERT INTO slpos (sid, disk, dseq, fsize, start, mtime, fpath) VALUES (?, ?, :i1, :i2, :i3, ?, ?)";
	} else {
		if(st <= 0 || state[0] != 'D') {
			fclose(fp);
			return -1;
		}
		const void *sbind[] = {fpath};
		dbcmd.cmd =
			"SELECT disk, dseq, fsize, start, mtime, fpath FROM slpos WHERE sid = ? ORDER BY disk, dseq";
		SQL(&dbcmd, bind, sbind);
	}
	sqint dseq = 0, fsize, sliceSize = 0, dsize[config.disks] = {0}, files = 0, eohSize = 0, eohFiles = 0, fill;
	char buf[PATH_MAX], *tok[HEADER_TOKENS_MAX], diskId[8] = {0}, *p;
	int n, rv = -1, hs = 0;
	if(intoDB)
		BEGIN();
	if(fgets(buf, sizeof(buf), fp) == NULL)
		goto end_proc;
	// check first header line
	n = strlen(buf);
	hs += n;
	n = decodeHeaderLine(buf, tok);
	if(n != 4 || strcmp(tok[0], "ParitySlice:") != 0 || strcmp(tok[1], fpath) != 0 || strcmp(tok[2], "fill:") != 0)
		goto end_proc;
	fill = atol(tok[3]);
	while(fgets(buf, sizeof(buf), fp) != NULL) {
		n = strlen(buf);
		hs += n;
		decodeHeaderLine(buf, tok, 4);
//		removeLastNewline(buf);
//		n = split(buf, tok, 5, BLANKS, 4);
		if(strcmp(tok[0], EOH) == 0) {
			// EOH line
			eohSize = atol(tok[1]);
			eohFiles = atol(tok[2]);
			rtrim(tok[3]);
			if(strcmp(tok[3], fpath) != 0)
				goto end_proc;
			break;
		}
		// next file
		COPY_FLD(diskId, tok[3]);
		p = strchr(diskId, getPathSeparator());
		if(p == NULL)
			goto end_proc;
		*p = 0;
		int didx = getDiskIndex(diskId);
		if(didx < 0 || didx >= config.disks)
			goto end_proc;
		fsize = atol(tok[0]);
		dseq++;
		files++;
		if(intoDB) {
			const void *vbind[] = {fpath, diskId, &dseq, &fsize, dsize + didx, tok[1], tok[3]};
			SQL(&dbcmd, bind, vbind);
			SQL(&dbcmd, step);
		} else {
			sqint db_seq, db_fsize, db_start;
			const void *dvars[] = {NULL, &db_seq, &db_fsize, &db_start, NULL, NULL};
			if(SQL(&dbcmd, step, dvars) <= 0)
				goto end_proc;
			if(db_fsize != fsize)
				goto end_proc;
			const char **vars = (const char **) dvars;
			if(strcmp(diskId, vars[0]) || strcmp(vars[4], tok[1]) || strcmp(vars[5], tok[3]))
				goto end_proc;
			if(argPath == NULL) {
				// Check only variant. Not for RESTORE command
				if(pathMatchPattern(vars[5], -1)) {
					cs->filesWritten++;
					cs->bytesWritten += fsize;
					if(fsize > 0)
						storeHash(cs, didx, db_seq, tok[2]);
				}
			}
		}
		// After save fresh stats
		dsize[didx] += fsize;
		if(dsize[didx] > sliceSize)
			sliceSize = dsize[didx];
	}
	if(files != eohFiles || sliceSize != eohSize)
		goto end_proc;
	verbose("Slice %s loaded. Header size: %d. Slice size: %s", fpath, hs, formatFileSize(sliceSize));
	// tail
	if(fseek(fp, sliceSize + hs, SEEK_SET) != 0)
		goto end_proc;
	if(fgets(buf, sizeof(buf), fp) == NULL || strncmp(buf, "ParityTail:", strlen("ParityTail:")) != 0)
			goto end_proc;
	n = decodeHeaderLine(buf, tok);
	if(n != 2 || strcmp(tok[1], fpath) != 0)
		goto end_proc;
//	rv = (hs > 0) ? hs : -1;
	if(hs > 0) {
		rv = hs;
		if(intoDB) {
			const char *state2 = "D";
			const void *bvar[] = {&sliceSize, &files, fpath, (void *) state2, &fill};
			SQL_EXEC("INSERT INTO slhead (ssize, files, sid, state, fill) VALUES (:i1, :i2, ?, ?, :i3)", NULL, bvar);
		} else {
			cs->headerSize = hs;
			cs->sliceSize = sliceSize;
			cs->filesCount = files;
			cs->fillFactor = fill;
		}
	}
end_proc:
	if(intoDB) {
		if(rv <= 0)
			ROLLBACK();
		else
			COMMIT();
	} else if(rv > 0)
		cs->filesWritten = cs->bytesWritten = 0;
	SQL(&dbcmd, finalize);
//	if(rv <= 0)
//		log_warn("Load slice header canceled. Inconsistent data in slice file: %s", fpath);
	if(fp)
		fclose(fp);
	return rv;
}

// return header size
int writeHeader(SLICE *cs, int mode = HEADER_WRITE) {
	SLICE_DISK *sd = getParityDisk(cs);	// parity drive
	sqint fsize, dseq, realSize;
	char buf[HEADER_BUF_SIZE];	// for header write
	char empty[SLICE_ID_SIZE + 1] = {0};
	char ts[TIMESTAMP_SIZE + 1] = {0};
	char hash[HASH_SIZE_DIGITS + 1] = {0};	// 64bit hash
	const void *dataVars[] = {NULL, &dseq, &fsize, NULL, NULL};
	int rv = 0, files = 0, nc;
	memset(hash, 'H', HASH_SIZE_DIGITS);
	uint hs = sprintf(buf, "ParitySlice: %*.*s  fill: %3ld\n", SLICE_ID_SIZE, SLICE_ID_SIZE, cs->sliceId, cs->fillFactor);
	int bufLen = hs;
	memset(ts, 'T', TIMESTAMP_SIZE);
	if((mode == HEADER_WRITE) && lseek(sd->fd, 0, SEEK_SET) < 0)
		return cancelGen(cs, "Seek failed.");
	SQL_COMMAND selectAll = {
		.cmd = "SELECT disk, dseq, fsize, fpath, mtime FROM slpos WHERE sid = ? ORDER BY disk, dseq"};	// for heder write
	const char *msg = "written";
	const void *binds[] = {cs->sliceId};
	SQL(&selectAll, bind, binds);
	while(SQL(&selectAll, step, dataVars) > 0) {
		// map variables
		char *disk = (char *) dataVars[0], *path = (char *) dataVars[3], *mtime = (char *) dataVars[4];
		int didx = getDiskIndex(disk);
		files++;
		if(mode == HEADER_WRITE) {
			char *dpath = path + strlen(disk) + 1;
			realSize = fileInfo(config.diskDesc[didx].path, dpath, ts);
			if(realSize < 0 || realSize != fsize || strcmp(ts, mtime) != 0) {
				// file not accessible or changed, ignore whole slice
				hs = cancelGen(cs, "Slice file changed. [%s]", path);
				goto end_proc;
			}
			if(fsize > 0) {
				findHash4seq(cs, didx, dseq, hash);
				if(strlen(hash) != HASH_SIZE_DIGITS) {
					hs = cancelGen(cs, "Hash value not computed. dseq: %d, file: [%s] ", dseq, (char *) dataVars[3]);
					goto end_proc;
				}
			}
			else
				memset(hash, '0', HASH_SIZE_DIGITS);
		}
		if(mode == HEADER_DUMP)
			nc = sprintf(buf + bufLen, "%*ld %s\n",	FILE_SIZE_DIGITS, fsize, path);
		else
			nc = sprintf(buf + bufLen, "%*ld %*.*s %*s %s\n",
					FILE_SIZE_DIGITS, fsize, TIMESTAMP_SIZE, TIMESTAMP_SIZE, ts, HASH_SIZE_DIGITS, hash, path);
		hs += nc;
		bufLen += nc;
		if(bufLen > HEADER_BUF_SIZE - 4096) {
			if(writeFD(sd->fd, buf, bufLen) != bufLen) {
				hs = cancelGen(cs, "Write slice header line failed");
				goto end_proc;
			}
			bufLen = 0;
		}
	}
	if(hs > 0) {
		char *sid = cs->sliceId;
		if(mode == HEADER_INIT) {
			memset(empty, 'S', SLICE_ID_SIZE);
			sid = empty;
		}
		nc = sprintf(buf + bufLen, "%s %*ld %*ld %*.*s", EOH,
			FILE_SIZE_DIGITS - 4, cs->sliceSize, FILE_SIZE_DIGITS / 2, cs->filesCount, SLICE_ID_SIZE, SLICE_ID_SIZE, sid);
		hs += nc;
		bufLen += nc;
		// align to 8 bytes
		rv = hs % 8;
		rv = 8 - rv;
		nc = sprintf(buf + bufLen, "%*s\n", rv - 1, "");
		hs += nc;
		bufLen += nc;
		if(writeFD(sd->fd, buf, bufLen) != bufLen) {
			hs = cancelGen(cs, "Write EOH failed");
			goto end_proc;
		}
	}
	if(mode == HEADER_INIT) {
		cs->headerSize = sd->fileOffset = hs;
		msg = "initiated";
	} else if(mode == HEADER_WRITE && hs != cs->headerSize) {
		hs = cancelGen(cs, "Inconsistent header size");
		goto end_proc;
	}
	verbose("Header for %s %s. Files: %d.  Size: %d\n", cs->sliceId, msg, files, hs);
end_proc:
	SQL(&selectAll, finalize);
	return hs;
}

//void removeSliceFile(const char *sid) {
//	char fpath[PATH_MAX];
//	sliceFileName(sid, fpath, "");
//	unlink(fpath);
//}

void recalculateNewfiles() {
	SQL_EXEC("UPDATE newfiles SET state = '' WHERE state = '*' AND name not in (SELECT fpath FROM slpos)");
}

int removeSlice(const char *sid, bool fileOnly) {
	if(!fileOnly) {
		const void *delVars[] = {sid};
		SQL_EXEC("DELETE FROM slpos WHERE sid = ?", NULL, delVars);
		SQL_EXEC("DELETE FROM slhead WHERE sid = ?", NULL, delVars);
	}
	unlinkAt(sid, PARITY_DIR);
	return 0;
}

static void markSync(bool start) {
	sqint rv = 0;
	if(start) {
		int file = openAt(SYNC_START_MARKER, O_CREAT | O_WRONLY, PARITY_DIR);
		if(file < 0)
			log_fatal("Can't create sync marker file: %s", SYNC_START_MARKER);
		if(futimens(file, NULL) < 0)
			log_fatal("Can't set modify time for sync marker file: %s", SYNC_START_MARKER);
		rv = fileInfo(file, NULL);
		close(file);
	} else
		unlinkAt(SYNC_START_MARKER, PARITY_DIR);
	const void *bv[] = {&rv};
	config.syncStart = rv;
	if(SQL_EXEC("UPDATE watchdog SET sync_start = :i", NULL, bv) <= 0)
		log_fatal("Can't mark sync in watchdog table");
	errno = 0;
}

void checkDuplicated() {
	const char *fname = "Duplicates.lst", *sid, *fpath;
	int dpl = SQL_EXEC("CREATE TEMP TABLE dtmp AS SELECT fpath FROM slpos GROUP BY fpath HAVING count(*) > 1");
	dpl = SQL_EXEC("CREATE TEMP TABLE dtmp2 AS SELECT DISTINCT sid, fpath FROM slpos WHERE fpath IN (SELECT fpath FROM dtmp)");
	SQL_COMMAND grp = {.cmd = "SELECT sid, fpath FROM dtmp2 ORDER BY sid"};
	dpl = 0;
	FILE *f = openAt(fname, "w", PARITY_DIR, true);
	fprintf(f, "List of slices with duplicated files\n");
	fprintf(f, "Slice                      file\n");
	fprintf(f, "-------------------------- ---------------------------------\n");
	char lastSid[SLICE_ID_SIZE + 1] = "";
	const void *dvars[] = {NULL, NULL};
	while(SQL(&grp, step, dvars) > 0) {
		sid = (const char *) dvars[0];
		fpath = (const char *) dvars[1];
		fprintf(f, "%s %s\n", sid, fpath);
		if(strcmp(sid, lastSid)) {
			dpl++;
			removeSlice(sid);
			strcpy(lastSid, sid);
		}
	}
	SQL(&grp, finalize);
	SQL_EXEC("DROP TABLE dtmp");
	SQL_EXEC("DROP TABLE dtmp2");
	if(dpl == 0)
		unlinkAt(fname, PARITY_DIR);
	else {
		PERROR(LOG_WARN, "Duplicates detected in slice positions. List in %s", fname);
		recalculateNewfiles();
		PERROR(LOG_WARN, "%d slices removed.", dpl);
		fprintf(f, "%d slices removed.\n", dpl);
	}
	fclose(f);
}

//static SQL_COMMAND verifySlice = {.cmd = "SELECT state, ssize, files FROM slhead WHERE sid = ?"};

// Compare data from slice file with database record
int verifySliceFiles() {
	sqint fssize, ffiles, count = 0;
	int rv = -1;
	char line[PATH_MAX], *p;
	sprintf(line, "%sslc-*-*.*", PARITY_DIR);
	SQL_EXEC("DROP TABLE IF EXISTS disk_slices; CREATE TEMP TABLE disk_slices (sid TEXT PRIMARY KEY, sl_size INTEGER, sl_files INTEGER)");
	SQL_COMMAND tmp_slc = {.cmd = "INSERT INTO disk_slices (sid, sl_size, sl_files) VALUES (?, :i1, :i2)"};
	log_info("Scan slices on backup disk");
	FILE *out = filterLines(line, "^EOH ", 1);
	if(out == NULL)
		return -1;
//	int canceled = 0, total = linesInFile(out, true);
//	log_info("%d slices found. Verify DB records", total);
//	if(total <= 0) {
//		pclose(out);
//		return 0;
//	}
	StatusBar sb = StatusBar("Load slices info into temporary table: %d");
//	char *sid = line;
//	const void *bvar[] = {NULL};
	const void *bvar[] = {NULL, &fssize, &ffiles};
	BEGIN();
	while(fgets(line, sizeof(line), out)) {
		p = strchr(line, ':');
		if(p) {
			*p++ = 0;
			p = decodeEOHline(p, &fssize, &ffiles);
		}
		bvar[0] = p;
		SQL(&tmp_slc, bind, bvar);
		SQL(&tmp_slc, step);
		count++;
		sb.show(1, 0L);
	}
	COMMIT();
//	log_info("Verified. %d errors", canceled);
	SQL(&tmp_slc, finalize);
	suid_pclose(out);
	const void *dvars[] = {&count};
//	log_info("%d slices info loaded. Verify ...", count);
	log_info("Verify ...", count);
	SQL_EXEC(
		"SELECT count(*) FROM disk_slices as d, slhead as h WHERE d.sid == h.sid AND (h.ssize != sl_size OR h.files != sl_files)",
		NULL, NULL, dvars);
	if(count != 0)
		log_warn("Inconsistent info in DB and on disk for %d slices", count);
	else {
		log_info("OK.");
		rv = 0;
	}
//	unlinkTmp(out);
	return rv;
}

static void updateOwners() {
	char line[PATH_MAX], *p, file[PATH_MAX];
	sprintf(line, "%sD*.acl", PARITY_DIR);
	FILE *f = filterLines(line, "^# file: \\|^# owner: \\|^D");
	DISK *disk = NULL;
	sqint updated = 0, st;
	if(f == NULL)
		log_fatal("can't select info from *.ACL files");
//	const void *dvars[] = {&updated};
	SQL_EXEC("DROP TABLE IF EXISTS file_owners; CREATE TEMP TABLE file_owners (path TEXT PRIMARY KEY, fowner TEXT)");
//	SQL_EXEC("SELECT count(*) FROM slpos", NULL, NULL, dvars);
	SQL_COMMAND upd = {.cmd = "INSERT INTO file_owners (path, fowner) VALUES (?, ?)"};
	log_info("Load owner ID for files ...");
	const void *bvars[] = {NULL, NULL};
//	StatusBar sb = StatusBar("Update slices position in database: %d/%d", updated, updated / 100);
	BEGIN();
	while(fgets(line, sizeof(line), f)) {
		if(isBlank(line))
			continue;
		p = strstr(line, ".acl:");
		if(p == NULL)
			log_fatal("Bad structure of records in 'grep' result");
		p += 5;
		if(*p == 'D') {
			int diskIdx = getDiskIndex(p);
			disk = config.diskDesc + diskIdx;
			continue;
		}
		if(!isPrefix("# file: ", p))
			log_fatal("Bad structure of records in 'grep' result");
		p += 8;
		disk2prefix(disk, p, true);
		strcpy(file, p);

		// Should be owner record
		if(fgets(line, sizeof(line), f) == NULL)
			log_fatal("Bad structure of records in 'grep' result");
		p = strstr(line, ".acl:");
		if(p == NULL)
			log_fatal("Bad structure of records in 'grep' result");
		p += 5;
		if(!isPrefix("# owner: ", p))
			log_fatal("Bad structure of records in 'grep' result");
		p += 9;
		bvars[0] = file;
		bvars[1] = p;
		SQL(&upd, bind, bvars);
		st = SQL(&upd, step);
		if(st == 1) {
			updated++;
//			sb.show(1, 0);
		}
	}
	COMMIT();
	SQL(&upd, finalize);
	suid_pclose(f);
	log_info("%d records loaded.", updated);
	log_info("Update owner column in slice positions ...", updated);
	updated = SQL_EXEC("UPDATE slpos SET owner = fowner FROM file_owners WHERE path = fpath");
	log_info("%d records updated", updated);
//	fclose(f);
}

static void refreshFromBackup(bool newOnly) {
	// Scan backup directory and optionally load headers into DB table
	if(config.sqliteMode[0] == 'r')	{ // rebuild DB option specified
		newOnly = false;
		markSync(false);
	} else { // Not rebuild
		if(!newOnly) {	// Run at program start. Fast verify
			time_t ct = time(NULL);
			if(config.verify || (ct - config.lastVerifyDB) >= VERIFYDB_DELTA)
			if(verifySliceFiles() == 0) {
				char cmd[128];
				sprintf(cmd, "UPDATE watchdog SET last_verified = %ld", ct);
				SQL_EXEC(cmd);
			}
		}
		if(fileInfo(PARITY_DIR, SYNC_START_MARKER) < 0)	// Not rebuild and no marker file.
			return;
		newOnly = true;
	}
	int h = 0, canceled = 0, k;
	char line[PATH_MAX], *ts;
	FILE *resp = generateRefreshList(newOnly);
	int total = linesInFile(resp);
	if(total > 0) {
		log_info("Scan backup directory for generated slice files");
		StatusBar sb = StatusBar("Slices processed: %d/%d", total, total / 100);
		k = sprintf(line, "%s", PARITY_DIR);
		while(fgets(line + k, sizeof(line) - k, resp)) {
			removeLastNewline(line + k);
			ts = strchr(line + k, ' ');
			*ts++ = 0;
			h = loadSliceHeader(line, NULL);
			if(h < 0) {
				if(canceled < 5)
					log_info("Inconsistent slice file: %s. Skipped", line);
				canceled++;
			}
			sb.show(1, 0L, " (canceled: %d)", canceled);
		}
		if(config.sqliteMode[0] != 'r')
			markSync(false);
		checkDuplicated();
	}
	fclose(resp);
}

void checkIfExists(bool checkOnly = false) {
	if(diskIsMounted()) {
		char *sid;
		int del = 0;
		sqint n;
		SQL_EXEC("CREATE TEMP TABLE etmp AS SELECT sid FROM slhead WHERE state LIKE 'D%'");
		log_info("Checking if slices in DB exists on disk ...");
		SQL_COMMAND grp = {.cmd = "SELECT sid FROM etmp ORDER BY sid"};
		const void *dvars[] = {NULL};
		if(!checkOnly)
			BEGIN();
		while(SQL(&grp, step, dvars) > 0) {
			sid = (char *) dvars[0];
			n = fileInfo(PARITY_DIR, sid);
			if(n < MIN_SLICE_SIZE) {
				if(del < 5)
					log_info("Slice %s not on backup disk!", sid);
				if(!checkOnly)
					removeSlice(sid, false);
				del++;
			}
		}
		SQL(&grp, finalize);
		if(del > 0) {
			const char *rem = (checkOnly) ? "" : "Removed from DB";
			log_info("%d slices not on backup disk. %s", del, rem);
		}
		if(del && !checkOnly)
			recalculateNewfiles();
		if(!checkOnly)
			COMMIT();
		SQL_EXEC("DROP TABLE etmp");
	}
}

static void checkAndFix(bool newOnly) {
	// check if generation some backup files was not interrupted
	// Remove garbage empty slice files and slice files with INITIAL_PREFIX
	if(diskIsMounted()) {
		refreshFromBackup(newOnly);
		if(config.sqliteMode[0] == 'r')
			updateOwners();
	}
	else if(config.syncStart) {
		log_warn("Sync command not properly finished. Sqlite DB can be inconsistent. Mount backup disk if you want to fix.");
	}
}

static sqlite3 *startSqlite(const char *dbName = NULL) {
	if(dbName == NULL)
		dbName = defaultDB;
	bool defDb = strcmp(dbName, defaultDB) == 0;
	if(defDb) {
		sqlite3_config(SQLITE_CONFIG_SERIALIZED);
		if(config.sqliteMode[0] == 'r') {
			log_info("Rebuild Sqlite database %s%s", config.workDir, defaultDB);
			unlinkAt(defaultDB, config.workDir);
		}
	}
	sqlite3 *db = openDB(dbName, config.workDir);
	if(defDb) {
		sqint start = getTimestamp(NULL, NULL);
		const void *dvars[] = {&config.progStart, &config.syncStart, &config.lastVerifyDB};
		setDefaultBase(db);
		SQL_EXEC("CREATE TABLE IF NOT EXISTS watchdog (prog_start INTEGER, sync_start INTEGER, last_verified INTEGER DEFAULT 0)");
		if(SQL_EXEC("SELECT prog_start, sync_start, last_verified FROM watchdog LIMIT 1", NULL, NULL, dvars) <= 0) {
			config.progStart = start;
			SQL_EXEC("INSERT INTO watchdog (prog_start, sync_start) VALUES (:i1, :i2)",
				NULL, dvars);
		} else {
			if(config.progStart && config.sqliteMode[0] != 'f') {	// Not force-db, rebuild?
				if(diskIsMounted()) {
					PERROR(LOG_INFO, "DB (%s%s) not closed properly.", config.workDir, defaultDB);
					config.sqliteMode = "r";
					closeDB(db);
					return startSqlite(defaultDB);
				}
				PERROR(LOG_FATAL, "DB (%s%s) not closed properly. Archive disk not mounted. Mount backup disk or use --sqliteMode=f",
					config.workDir, defaultDB);
			}
			config.progStart = start;
			SQL_EXEC("UPDATE watchdog SET prog_start = :i", NULL, dvars);
		}
		SQL_EXEC(
			"CREATE TABLE IF NOT EXISTS newfiles "
			"(inode INTEGER, size INTEGER, type TEXT, seq INTEGER, mtime TEXT, disk TEXT, "
			"state TEXT DEFAULT '', owner TEXT, name TEXT PRIMARY KEY)"
			);
		SQL_EXEC(
			"CREATE TABLE IF NOT EXISTS slpos ( "
			"sid TEXT, disk TEXT, dseq INTEGER, fsize INTEGER, start INTEGER, state text DEFAULT '', "
			"mtime TEXT, hash TEXT default '', fpath TEXT, owner TEXT, PRIMARY KEY(sid, disk, dseq))"
		);
		SQL_EXEC("CREATE TABLE IF NOT EXISTS slhead "
			"(ssize INTEGER, files INTEGER, sid TEXT, state TEXT DEFAULT '', fill INTEGER)");
		SQL_EXEC("CREATE TABLE IF NOT EXISTS links (ftype TEXT, link_name TEXT PRIMARY KEY, target TEXT)");
	}
	return db;
}

int openWorkFile(SLICE *cs, int diskIdx) {
	SLICE_DISK *sd = cs->disks + diskIdx;
	const char *fname = (isParity(diskIdx)) ? cs->sliceId : fileBasename(cs->savePath);
	sd->fd = openTempFD(fname, NULL, false);
	if(sd->fd < 0)
		cancelGen(cs, "Can't open temporary file: [%s]", fname);
	return sd->fd;
}

// Expand 'D<idx>' to disk mount point
void prefix2path(const char *spath, char *dpath) {
	int idx = getDiskIndex(spath);
	dpath[0] = 0;
	if(idx >= 0) {
		const char *p = strchr(spath, getPathSeparator());
		sprintf(dpath, "%s%s", config.diskDesc[idx].path, p);
	} else
		strcpy(dpath, spath);
}

// Convert disk mount point to 'D<idx>'
void path2prefix(char *path, bool removeNL) {
	for(int i = 0; i < config.disks; i++) {
		DISK *dsk = config.diskDesc + i;
		if(isPrefix(dsk->path, path)) {
			disk2prefix(dsk, path, removeNL);
		}
	}
}

static void restoreSpecial(const char *destPath) {
	char lpath[PATH_MAX], buf[PATH_MAX], *link_name, *p, *ftype;
	const char *restoreRoot;
	const void *dvar[] = {NULL, NULL, NULL};
	int emul;
	FILE *rap = getTCA()->raport;
	SQL_COMMAND sel = {.cmd = "SELECT link_name, target, ftype FROM links"};
	SLICE local_sl = {.raport = rap};
	while(SQL(&sel, step, dvar) > 0) {
		// get full paths
		link_name = (char *) dvar[0];
		if(isPrefix(config.restoreDiskId, link_name) && pathMatchPattern(link_name, config.restoreOwner)) {
			ftype = (char *) dvar[2];
			strcpy(local_sl.savePath, link_name);
			strcpy(lpath, link_name);
			// restore full path
			restoreRoot = restoreDirs(lpath, &emul, ftype[0] == 'd');
			if(restoreRoot == NULL || emul < 0) {
				const char *msg = (emul < 0) ? "#Can't restore dirs owners on path." : "#Can't restore directory.";
				cancelGen(&local_sl, msg);
				if(restoreRoot == NULL)
					continue;	// dir not restored
			}
			// Full path restored create target object
			p = strchr(link_name, getPathSeparator()) + 1;
			if(ftype[0] == 'l' || ftype[0] == 'h') {
				// Link
				char *target = (char *) dvar[1];
				strcpy(lpath, destPath);
//				int k = strlen(lpath);
				strcat(lpath, p);
				prefix2path(target, buf);
				target = buf;
				int st = (ftype[0] == 'l') ? symlink(target, lpath) : link(target, lpath);
				if(st && errno != EEXIST) {
					cancelGen(&local_sl, "#Can't restore link -> %s", target);
					local_sl.genCanceled = false;
				}
			}
		}
	}
	SQL(&sel, finalize);
}

int saveWorkFile(SLICE *cs, int diskIdx) {
	int rv = 0, emul;
	SLICE_DISK *sd = cs->disks + diskIdx;
	if(sd->fd >= 0) {
		char wfile[PATH_MAX], dpath[PATH_MAX + 8];
		getFilePath(sd->fd, wfile);
		if(config.dry == 'c') {
			close(sd->fd);
			sd->fd = -1;
			unlink(wfile);	// Compare only
			return 0;
		}
		if(!cs->genCanceled) {
			int renameFlag = 0;
			char ps = getPathSeparator();
			const char *fdir = config.diskDesc[diskIdx].path, *path, *ext = "";
			if(isParity(diskIdx)) {
				// save slice file
				path = cs->sliceId;
				if(config.dry)
					ext = HEADER_EXT;
			} else {
				// restore file timestamps
				timespec mt[2] = {0};
				str2timestamp(sd->file.mtime, mt);
				mt[1] = mt[0];
				if(futimens(sd->fd, mt) < 0)
					errno = 0;
				if(config.restoreDest)
					fdir = config.restoreDest;
				if(config.restoreFlat) {
					path = fileBasename(cs->savePath);
					renameFlag = RENAME_NOREPLACE;
				} else {
					path = cs->savePath;
					if(config.replace <= 0 && fileInfo(fdir, strchr(cs->savePath, ps) + 1, NULL) > config.replace)
						return cancelGen(cs, "File exists");	// file exists
					fdir = restoreDirs(path, &emul);
					if(fdir == NULL)
						return cancelGen(cs, "Can't restore dirs on path: [%s]", path);
					if(emul < 0)
						cancelGen(cs, "#Can't restore dirs owners on path above.");
					path = strchr(path, ps) + 1;
				}
			}
			close(sd->fd);
			sd->fd = -1;
			sprintf(dpath, "%s%s%s", fdir, path, ext);
			rv = renameat2(0, wfile, 0, dpath, renameFlag);
			if(rv != 0) {
				path = strrchr(wfile, ps);
				cancelGen(cs, "Can't rename work file [%s] to restore directory. Duplicate flat path?", path);
			} else {
				if(config.cmd == RESTORE && config.acl && restoreACL(cs->savePath, dpath) < 0) {
					strcpy(wfile, "#Can't restore file owner. ");
					if(config.restoreACLsupported)
						strcat(wfile, "Owner permissions emulated by ACL");
					cancelGen(cs, wfile);
				}
				cs->genCanceled = false;
			}
		}
	}
	return rv;
}

int initSliceFile(SLICE *cs) {
	if(openWorkFile(cs, PARITY_DISK - 1) < 0)
		return 0;
	verbose("Write slice %s to disk", cs->sliceId);
	int md = (config.dry == 'h') ? HEADER_DUMP : HEADER_INIT;
	int rv = writeHeader(cs, md);
	return rv;
}

int exitSliceFile(SLICE *cs) {
	char tail[256];
	int rv = -1;
	SLICE_DISK *sd = getParityDisk(cs);	// parity drive
	if(cancelRequest(cs))
		goto end_proc;
	if(!cs->genCanceled && config.dry != 'h') {
		rv = writeHeader(cs, HEADER_WRITE);
		if(rv > 0 && !config.dry) {
			int bufLen = sprintf(tail, "ParityTail: %*s", SLICE_ID_SIZE, cs->sliceId);
			off_t end = lseek(sd->fd, 0, SEEK_END);
			if((end != (cs->sliceSize + cs->headerSize)) || writeFD(sd->fd, tail, bufLen) != bufLen) {
				rv = cancelGen(cs, "Write tail failed.");
				goto end_proc;
			}
		}
	}
	rv = saveWorkFile(cs, cs->disk2write);
end_proc:
	if(sd->fd > 0) {
		close(sd->fd);
		sd->fd = -1;
		cs->savePath[0] = 0;
	}
	return rv;
}

void allocHashArray(SLICE *cs) {
	cs->hashCount = 0;
	if(cs->hashArray) {
		if(cs->filesCount <= cs->hashSize)
			return;
		// its too small, realloc
		free(cs->hashArray);
	}
	cs->hashArray = (HASH_POS *) malloc(cs->filesCount * sizeof(HASH_POS));
	if(cs->hashArray == NULL)
		log_fatal("No memory.");
	cs->hashSize = cs->filesCount;
}

void initSliceStructure(SLICE *cs) {
	SLICE_DISK *sd;
	for(int i = 0; i < PARITY_DISK; i++) {
		sd = cs->disks + i;
		sd->dataBuf = (char *) malloc(config.fileBufferSize);
		sd->fd = -1;
		SLICE_FILE *cf = &sd->file;
		cf->hashState = allocHashState();
		if(sd->dataBuf == NULL || cf->hashState == NULL)
			log_fatal("Malloc failed");
		bzero(sd->dataBuf, config.fileBufferSize);
	}
}

void statusBar() {
	static sqint bspeed;	// last, temporary speed
	TCA *tca = getTCA();
	if(tca->operationCanceled || (!showStatusBar && tca->files2process != tca->filesProcessed))
		return;
	sqint dataDiff = tca->dataProcessed - tca->lastProcessed;
	int ct = time(NULL);
	if(dataDiff > tca->dataTick || ct != tca->lastTime || tca->files2process == tca->filesProcessed) {
		int rtime  = ct - tca->start, etas = 0;
		tca->lastTime = ct;
		tca->lastProcessed = tca->dataProcessed;
		char eta[64] = "", speed[64] = "", sizes[64], line[256] = "";
		double dp = double(tca->dataProcessed) / double(tca->data2process);
		if(rtime) {
			if(tca->files2process == tca->filesProcessed || tca->speedStartTime == tca->start) {	// mean speed
				bspeed = tca->dataProcessed / rtime;
			} else {	// current speed
				sqint bt = tca->dataProcessed - tca->speedBytesStart;
				int td = ct - tca->speedStartTime;
				if(bt > 2 * sqint(GIGA) || td > 10) {
					// refresh temporary speed
					bspeed = bt / td;
					tca->speedStartTime = ct;
					tca->speedBytesStart = tca->dataProcessed;
				}
			}
		}
		if(bspeed)
			etas = (tca->data2process - tca->dataProcessed) / bspeed;
		int proc = round(dp * 100.0), mspeed = bspeed / (1024 * 1024);	// in MB
		sprintf(speed, "%3d MB/s", mspeed);
//		int k = sprintf(line, "Run time: %s    files: %d/%d", runtime, tca->filesProcessed, tca->files2process);
		int k = sprintf(line, "Process files: %d/%d", tca->filesProcessed, tca->files2process);
		if(tca->filesSkipped)
			k += sprintf(line + k, " (skipped: %d)", tca->filesSkipped);
		if(config.dry != 'h') {
			sec2hours(etas, eta);
//			log_info("dp: %lf, rtime: %d  Speed: %d", dp, rtime, bspeed);
			int n = formatSize(tca->dataProcessed, sizes, false);
			strcat(sizes, " / ");
			formatSize(tca->data2process, sizes + n + 3, false);
			const char *speedmsg = (config.dry == 'r' || config.dry == 'c') ? "Dry read speed" : "Write speed";
			sprintf(line + k, "    bytes: %s (%d%%)    ETA: %s    %s: %s        ", sizes, proc, eta, speedmsg, speed);
		}
		if(showStatusBar)
			log_info("\r%s", line);
	}
}

void monitorRun(sqint bytes, int files = 1, int skipped = 0) {
	TCA *tca = getTCA();
	pthread_mutex_lock(&tca->mutex);
	tca->dataProcessed += bytes;
	tca->filesProcessed += files;
	tca->filesSkipped += skipped;
	if(showStatusBar || tca->filesProcessed >= tca->files2process)
		statusBar();
	pthread_mutex_unlock(&tca->mutex);
}

int createSliceFile(SLICE *cs) {
	cs->genCanceled = false;
	int rv = initSliceFile(cs);
	if(rv > 0) {
		sqint wrProcessed = 0;
		if(config.dry != 'h') {
			// process input data
			allocHashArray(cs);
			TCA *tca = getTCA();
			sqint bstep = tca->dataTick;
			while(true) {
				if(cancelRequest(cs))
					return -1;
				fillInputBuffers(cs);
				if(cs->bytesWritten > 0) {
					wrProcessed += cs->bytesWritten;
					cs->bytesWritten = 0;
					if(wrProcessed > bstep) {
						monitorRun(wrProcessed, 0, 0);
						wrProcessed = 0;
					}
				}
				if(computeParity(cs) <= 0)
					break;
			}
			if(wrProcessed > 0)
				monitorRun(wrProcessed, 0, 0);
		}
		exitSliceFile(cs);
	}
	if(cs->genCanceled)
		log_info("Slice file '%s' generation skipped." , cs->sliceId);
	return rv;
}

// thread function to save slices to backup disk
static void *slices2disk(void *jdata) {
	sqlite3 *db = openDB(defaultDB, config.workDir, 1);
	TCA *tca = getTCA();
	SLICE slc = {0}, *cs = &slc, wlc;
	slc.disk2write = PARITY_DISK - 1;
	SLICE_DISK *wdisk = wlc.disks + slc.disk2write;
	initSliceStructure(&slc);
	// prepare SQL statement for future use
	sqint rowid;
	SQL_COMMAND get_sid = {.cmd = "SELECT sid, ssize, files, fill FROM slhead WHERE rowid = :i", .db = db};
	const void *vars[] = {NULL, &slc.sliceSize, &slc.filesCount, &slc.fillFactor};
	const void *binds[] = {&rowid};
	cs->selectAll.cmd =
		"SELECT disk, dseq, fsize, fpath, mtime FROM slpos WHERE sid = ? ORDER BY disk, dseq";	// for heder write
	cs->selectNext.cmd =
		"SELECT dseq, fsize, fpath FROM slpos WHERE sid = ? AND disk = ? AND dseq > :i ORDER BY dseq LIMIT 1"; // for generate parity
	cs->selectAll.db = cs->selectNext.db = db;
	SQL(&cs->selectAll, prepare, NULL);
	SQL(&cs->selectNext, prepare, NULL);
	int canceled;
	while((rowid = (sqint) tca->queue->pop()) != 0 && ! cancelRequest(NULL)) {
		SQL(&get_sid, bind, binds);
		if(SQL(&get_sid, step, vars) > 0) {
			memcpy(&wlc, cs, sizeof(SLICE));	// clear structure (copy from saved initialized)
			strcpy(wlc.sliceId, (char *) vars[0]);
			wdisk->file.bytes2transfer = wdisk->file.fileSize = cs->sliceSize;	// parity data size in slice file
			createSliceFile(&wlc);
			canceled = (wlc.genCanceled) ? 1 : 0;
			if(canceled && wlc.outOfSpace) {
				tca->operationCanceled = true;
				log_warn("Out of space on backup disk");
				break;
			}
			monitorRun(0, 1, canceled);
		}
	}
	SQL(&get_sid, finalize);
	SQL(&cs->selectAll, finalize);
	SQL(&cs->selectNext, finalize);
	closeDB(db);
	freeSlice(cs);
	return NULL;
}

sqint filterByPath(const char *where, int *files) {
	char *path, cmd[256];
	const char *wand = "";
	if(where == NULL)
		where = "";
	else
		wand = " AND ";
	sprintf(cmd, "SELECT slpos.fpath, slhead.rowid, slpos.fsize FROM slpos "
		"INNER JOIN slhead ON slpos.sid == slhead.sid %s%s ORDER BY slhead.rowid", wand, where);
	sqint rowid, flen, sumlen = 0, lastRowid = 0, fcount = 0;
	SQL_COMMAND filter = {.cmd = cmd};
	const void *dvars[] = {NULL, &rowid, &flen};
	while(SQL(&filter, step, dvars) > 0) {
		path = (char *) dvars[0];
		if(pathMatchPattern(path, config.restoreOwner)) {
			if(rowid != lastRowid) {
				lastRowid = rowid;
				if(jobFilter.push((void *) rowid) < 0) {
					log_warn("Filter result exceeded queue size (%d)", jobFilter.getCount());
					break;
				}
			}
			sumlen += flen;
			fcount++;
		}
	}
	SQL(&filter, finalize);
	if(sumlen == 0 && fcount == 0) {	// nothing
		PERROR(LOG_WARN, "No data in archive for specified filters");
	}
	if(files)
		*files = fcount;
	return sumlen;
}

sqint load2queue(const char *where, int *nfiles = NULL, int maxElems = 0) {
	char cmd[256];
	sqint bytes = 0, rowid, filesCount = 0, fsize, files;
	if(where == NULL) {
		sprintf(cmd, "SELECT slhead.rowid, files, ssize FROM slhead WHERE state = ''");
	} else {
		sprintf(cmd, "SELECT slhead.rowid, count(*), sum(slpos.fsize) AS s FROM slhead, slpos "
			"WHERE slhead.sid = slpos.sid AND slhead.state LIKE 'D%%' AND %s GROUP BY slhead.rowid ORDER BY s DESC", where);
	}
	SQL_COMMAND sel = {.cmd = cmd};
	const void *vars[] = {&rowid, &files, &fsize};
	int nelem = 0;
	while(SQL(&sel, step, vars) > 0) {
		filesCount += files;
		bytes += fsize;
		if(jobFilter.push((void *) rowid) < 0) {
			log_warn("Job queue too small (%d).", jobFilter.getCount());
			break;
		}
		if(maxElems) {
			nelem++;
			if(nelem >= maxElems)
				break;
		}
	}
	if(nfiles)
		*nfiles = filesCount;
	SQL(&sel, finalize);
	return bytes;
}

static void initTCA(JobQueue *queue, int s2process, sqint d2process, const char *rapName) {
	TCA *tca = getTCA();
	tca->files2process = s2process;
	tca->data2process = d2process;
	tca->queue = queue;
	tca->start = tca->lastTime = tca->speedStartTime = time(NULL);
	tca->dataTick = d2process / 100;
	if(tca->dataTick > GIGA)
		tca->dataTick = GIGA;
	else if(tca->dataTick < MEGA)
		tca->dataTick = MEGA;
	int omask = umask(RUMASK);
	tca->raport = openAt(rapName, "w+", config.workDir, true);
	umask(omask);
	fprintf(tca->raport, "File to write\n-->Exception info\n\n");
	pthread_mutex_init(&tca->mutex, NULL);
}

static void exitTCA() {
	if(globalTCA.raport) {
		sqint files = linesInFile(globalTCA.raport, true);
		char path[PATH_MAX];
		getFilePath(fileno(globalTCA.raport), path);
		fclose(globalTCA.raport);
		globalTCA.raport = NULL;
		if(files > 3) {
			const char *bn = fileBasename(path);
			files = (files / 3) - 1;
			PERROR(LOG_WARN, "%d exception while create files. Inspect '%s' for details", files, bn);
		}
		else
			unlink(path);
	}
	globalTCA.start = 0;
}

int saveSlices() {
//	const char *dpath = config.diskDesc[PARITY_DISK - 1].path;
	int maxElem = 0;
	sqint n = load2queue(NULL, NULL, maxElem);
	if(n <= 0)
		return 0;
	markSync(true);
	JobQueue jq(&jobFilter, JobQueue::QUEUE_END_OF_DATA, config.maxThreads > 1);
	initTCA(&jq, jobFilter.getCount(), n, UNSAVED);
	if(!config.dry) {
		unlinkAt(UNRECOVER_FILE, config.workDir);
		unlinkAt(CHANGED_FILE, config.workDir);
	}
	int thr = config.maxThreads = MIN(config.maxThreads, n);
	log_info("Reserve %d * %d MB memory for disk IO buffers", (config.disks + 1) * MAX(thr, 1), config.fileBufferSize / MEGA);
	if(thr <= 1) {
		slices2disk(NULL);
	} else {
		int i;
		log_info("Start %d threads", thr);
		pthread_t thread[thr] = {0};
		initAIO();
		for(i = 0; i < thr; i++) {
			if(pthread_create(thread + i, NULL, slices2disk, NULL)) {
				thread[i] = 0;
				break;
			}
		}
		// wait for finish
		for (i = 0; i < thr; ++i) {
			if(thread[i])
				pthread_join(thread[i], NULL);
		}
	}
	exitTCA();
	checkAndFix(true);
	return 0;
}

//void listArchive() {
//	sqint fill, rowid, ssize;
//	int files;
//	(config.patternsCount > 0 || config.owner) ? filterByPath(NULL, &files) : load2queue(NULL, &files);
//	const void *dvars[] = {NULL, &ssize, &files, NULL, &fill};
//	const void *binds[] = {&rowid};
//	SQL_COMMAND list = {.cmd = "SELECT sid, ssize, files, state, fill FROM slhead WHERE rowid == :i"};
//	FILE *oldVerb = verboseFile;
//	verboseFile = stdout;
//	while((rowid = (sqint) jobFilter.pop())) {
//		SQL(&list, bind, binds);
//		if(SQL(&list, step, dvars) > 0)
//			printSlice((char *) dvars[0], ssize, files, (char *) dvars[3], fill);
//	}
//	verboseFile = oldVerb;
//	SQL(&list, finalize);
//}
//
int restoreFromSlice(SLICE *cs) {
	allocHashArray(cs);
	// Verify slice header. Compute start parity data
	int headerSize = loadSliceHeader(NULL, cs);
	if(headerSize <= 0) {
		cs->filesCanceled = cs->filesWritten;
		return 0;
	}
	SLICE_DISK *sd = getParityDisk(cs);
	sd->fd = openAt(cs->sliceId, O_RDONLY, PARITY_DIR);
	if(sd->fd < 0 || lseek(sd->fd, headerSize, SEEK_SET) != headerSize) {
		if(sd->fd >= 0) {
			close(sd->fd);
			sd->fd = -1;
		}
		return log_error("Can't open or seek on slice file %s", cs->sliceId);
	}
	sqint wrProcessed = 0;
	int filesProc = 0, canceled = 0;
	TCA *tca = getTCA();
	sqint bstep = tca->dataTick;
//	sleep(2);
	while(true) {
		if(cancelRequest(cs))
			return -1;
		if(fillInputBuffers(cs) < 0)
			break;	// EOF for slice
		wrProcessed += cs->bytesWritten;
		filesProc += cs->filesWritten;
		canceled += cs->filesCanceled;
		cs->bytesWritten = cs->filesWritten = cs->filesCanceled = 0;
		if(wrProcessed > bstep) {
			monitorRun(wrProcessed, filesProc, canceled);
			wrProcessed = filesProc = canceled = 0;
		}
		if(cs->genCanceled)
			cs->genCanceled = false;
		else if(computeParity(cs) < 0)
			break;
	}
	cs->bytesWritten += wrProcessed;
	cs->filesWritten += filesProc;
	cs->filesCanceled += canceled;
	return 0;
}

// thread function to restore files from backup
void *restoreFromDisk(void *env) {
	TCA *tca = getTCA();
	SLICE loc_slc = {.disk2write = getDiskIndex(config.restoreDiskId)};
	loc_slc.raport = tca->raport;
	SLICE wlc, *cs = &loc_slc;
	sqint rowid;
	const void *binds[] = {&rowid};
	initSliceStructure(cs);
	SQL_COMMAND get_sid = {.cmd = "SELECT sid, ssize, files, fill FROM slhead WHERE rowid = :i"};
	cs->selectNext.cmd =
		"SELECT dseq, fsize, fpath, start, mtime, state FROM slpos WHERE sid = ? AND disk = ? AND dseq > :i ORDER BY dseq LIMIT 1"; // for generate parity
	SQL(&cs->selectNext, prepare, NULL);
	SQL(&get_sid, bind, binds);
	const void *vars[] = {NULL, &cs->sliceSize, &cs->filesCount, &cs->fillFactor};
	while((rowid = (sqint) tca->queue->pop()) && ! cancelRequest(NULL)) {	// select slice to process
		SQL(&get_sid, bind, binds);
		if(SQL(&get_sid, step, vars) > 0) {
			memcpy(&wlc, cs, sizeof(SLICE));	// clear structure (copy from saved initialized)
			COPY_FLD(wlc.sliceId, vars[0]);
			SLICE_DISK *sd = getParityDisk(&wlc);
			sd->file.bytes2transfer = sd->file.fileSize = cs->sliceSize;	// parity data size in slice file (max file size)
			restoreFromSlice(&wlc);
			monitorRun(wlc.bytesWritten, wlc.filesWritten, wlc.filesCanceled);
		}
	}
	SQL(&get_sid, finalize);
	SQL(&cs->selectNext, finalize);
	freeSlice(cs);
	return 0;
}

void disk2prefix(DISK *disk, char *path, bool removeNL) {
	char *diskId = disk->id;
	int idLen = strlen(disk->id);
	int prefixLen = strlen(disk->path);
	int pl = strlen(path);
	if(removeNL)
		pl = removeLastNewline(path);
	if(prefixLen <= idLen)
		memmove(path + idLen - prefixLen + 1, path, pl + 1);
	char *path2 = path + prefixLen - idLen - 1;
	memcpy(path2, diskId, idLen);
	memmove(path, path2, strlen(path2) + 1);
//	return path;
}

int convertACL(int didx) {
	static const char *pref = "# file: ";
	static int prefLen = strlen(pref);
	DISK *disk = config.diskDesc + didx;
	char *diskId = disk->id, line[PATH_MAX], path[PATH_MAX];
	sprintf(line, "%s%s", config.workDir, diskId);
	strcpy(path, line);
	strcat(line, ".tmp");
	FILE *f = fopen(line, "r");
	if(f == NULL)
		log_fatal("Unable open data file: %s", line);
	strcat(path, ".acl");
	FILE *fout = fopen(path, "w");
	if(fout == NULL)
		log_fatal("Unable open data file: %s", path);
	while(fgets(line, sizeof(line), f) != NULL) {
		if(isPrefix(pref, line)) {
			disk2prefix(disk, line + prefLen, false);
		}
		if(fputs(line, fout) < 0)
			log_fatal("Unable write data to file: %s", path);
	}
	fclose(f);
	fclose(fout);
	sprintf(line, "%s%s.tmp", config.workDir, diskId);
	unlink(line);
	return 0;
}

static void prepareForSync() {
	// remove outdated slices (contained modified or removed files)
	SQL_COMMAND del = {.cmd = "SELECT sid FROM slhead WHERE state LIKE '%*'"};
	log_info("Search for outdated slices ...");
	const void *dvars[] = {NULL};
	int removed = 0;
	cleanDir(PARITY_DIR, "*.header");
	while(SQL(&del, step, dvars) > 0) {
		removeSlice((char *) dvars[0], true);
		removed++;
	}
	SQL(&del, finalize);
	if(removed > 0) {
		log_info("%d outdated slices removed.", removed);
		BEGIN();
		SQL_EXEC(
			"DELETE FROM slpos WHERE sid IN (SELECT sid FROM slhead WHERE state LIKE '%*');"
			"DELETE FROM slhead WHERE state LIKE '%*';"
		);
		recalculateNewfiles();
		COMMIT();
	}
	checkIfExists();
	log_info("Search finished.");
}

void createTempTables(bool both) {
	char addWhere[128] = "", cmd[PATH_MAX];
	if(config.userId != config.euserId)
		sprintf(addWhere, " AND p.owner = '%d'", config.userId);
	SQL_EXEC("DROP TABLE IF EXISTS tmp; DROP TABLE IF EXISTS tmp2");
	sprintf(cmd, "CREATE TEMP TABLE tmp AS SELECT p.disk, p.start, p.fsize, p.sid, p.fpath, p.mtime FROM slpos p, slhead h "
		"WHERE h.sid = p.sid AND h.state = 'D*' AND p.state = '*' %s ORDER BY p.sid, p.disk, p.start", addWhere);
	SQL_EXEC(cmd);
	if(both) {
		SQL_EXEC("CREATE TEMP TABLE tmp2 AS SELECT DISTINCT p.disk, p.start, p.fsize, p.sid, p.fpath, p.mtime FROM tmp t, slpos p "
				"WHERE t.sid = p.sid and t.disk != p.disk AND p.start <= t.start + t.fsize AND t.start <= p.start + p.fsize "
				"ORDER BY p.sid, p.disk, p.start");
	}
}

int listResult(FILE *rpt, SQL_COMMAND *sel) {
	sqint fsize, start, sum = 0;
	int count = 0;
	char sbuf[32], ldisk[8] = "", lsid[SLICE_ID_SIZE + 8] = "", dsid[sizeof(lsid)], *p, *path;
	sprintf(dsid, "             -              ");
	const void *dvars[] = {NULL, NULL, NULL, NULL, &fsize, &start};
	fprintf(rpt, "Start in slice       Size   Slice modify time     Slice                        Path\n");
	fprintf(rpt, "-------------- ----------   --------------------  ---------------------------- -----------------------------\n");
	while(SQL(sel, step, dvars) > 0) {
		path = (char *) dvars[0];
		if(!pathMatchPattern(path, config.restoreOwner))
			continue;
		p = (char *) dvars[3];
		formatSize(fsize, sbuf, false);
		if(strcmp(ldisk, (char *) dvars[1])) {
			if(ldisk[0])
				fprintf(rpt, "\n");
			strcpy(ldisk, (char *) dvars[1]);
		}
		if(strcmp(lsid, p)) {
			strcpy(lsid, (char *) dvars[3]);
		} else if(lsid[0])
			p = dsid;
		fprintf(rpt, "%14ld %11.11s  %s  %s %s\n", start, sbuf, (char *) dvars[2], p, path);
		count++;
		sum += fsize;
	}
	fprintf(rpt, "-------------- ----------   --------------------\n");
	formatSize(sum, sbuf, false);
	fprintf(rpt, "               %11.11s  Files: %d\n", sbuf, count);
	SQL(sel, finalize);
	if(count)
		log_info("List created. %d files ( %s) in list.", count, sbuf);
	else
		log_info("List is empty. Delete result file.");
	return count;
}

FILE *openRaportFile(const char *rname) {
	char wh[PATH_MAX];
	const char *dir = config.workDir;
	if(config.userId != config.euserId) {
		dir = getcwd(wh, sizeof(wh));
		addPathSeparator(wh);
	}
	FILE *rpt = openAt(rname, "w", dir, true);
	if(dir == wh && config.euserId == 0)
		if(fchown(fileno(rpt), config.userId, 0) != 0)
			log_info("Can'y change file owner for file %s", rname);
	return rpt;
}

void listChanged() {
	char path[PATH_MAX];
	FILE *rpt = openRaportFile(CHANGED_FILE);
	createTempTables(false);
	SQL_COMMAND sel = {.cmd = "SELECT fpath, disk, mtime, sid, fsize, start FROM tmp ORDER BY disk, sid, start, fsize DESC"};
	log_info("Generate list of changed files into '%s' ...", getFilePath(fileno(rpt), path));
	fprintf(rpt, "List of changed files\n");
	if(listResult(rpt, &sel) <= 0)
		unlinkAt(CHANGED_FILE, config.workDir);
	fclose(rpt);
}

void listUnrecover() {
	char path[PATH_MAX];
	FILE *rpt = openRaportFile(UNRECOVER_FILE);
	getFilePath(fileno(rpt));
	createTempTables(true);
	log_info("Generate list of unrecoverable files into '%s' ...", getFilePath(fileno(rpt), path));
	fprintf(rpt, "List of potentially unrecovarable files\n");
	SQL_COMMAND sel = {.cmd = "SELECT fpath, disk, mtime, sid, fsize, start FROM tmp2 ORDER BY disk, sid, start, fsize DESC"};
//	if(diskIsMounted()) {
//		refreshFromBackup(true);
//	}
	if(listResult(rpt, &sel) <= 0)
		unlinkAt(UNRECOVER_FILE, config.workDir);
	fclose(rpt);
}

int restoreFromArchive() {
	char where[128];
	sprintf(where, "slpos.disk = 'D%d'", getDiskIndex(config.restoreDiskId) + 1);
	int files = 0;
	const char *dest = getRestorePath();
//	int didx = getDiskIndex(config.patterns[0]);
//	if(config.restoreDest == NULL)
//		dest = config.diskDesc[didx].path;
	sqint n = (config.patternsCount > 0 || config.regex[0] || config.restoreOwner >= 0) ? filterByPath(where, &files) : load2queue(where, &files);
	if(n > 0 || files > 0) {
		config.restoreACLsupported = isACLactive(dest);
//		clearWorkDir(dest);	// create/clear temporary directory on dest disk
		JobQueue jq(&jobFilter, JobQueue::QUEUE_END_OF_DATA, config.maxThreads > 1);
		int thr = (config.maxThreads < 1) ? 1 : config.maxThreads;
		log_info("Reserve %d * %d MB memory for disk IO buffers", (config.disks + 1) * thr, config.fileBufferSize / MEGA);
		initTCA(&jq, files, n, UNRESTORED);
		if(config.maxThreads >= 1)
			log_info("Start %d additional threads", config.maxThreads);
		if(config.maxThreads < 1) {
			restoreFromDisk(NULL);
		} else {
			int i, rv;
			pthread_t thread[config.maxThreads] = {0};
			initAIO();
			for(i = 0; i < config.maxThreads; ++i) {
				if((rv = pthread_create(thread + i, NULL, restoreFromDisk, NULL))) {
					thread[i] = 0;
					break;
				}
			}
			// wait for finish
			for (i = 0; i < config.maxThreads; ++i) {
				if(thread[i])
					pthread_join(thread[i], NULL);
			}
		}
	} else
		log_info("No files to restore!");
	if(!config.restoreFlat)
		restoreSpecial(dest);
	exitTCA();
	return 0;
}

void test() {
	const char *path = "/home/heniek/ec-work/parity-backup/backup-disk/.parity-backup";
	bool a = isACLactive(path);
	printf("ACL active for %s : %b\n", path, a);
	path = "/nas/heniek/dokumentacja";
	a = isACLactive(path);
	printf("ACL active for %s : %b\n", path, a);
}

// -------------------------------------------------------------
int main(int argc, const char** argv) {
	umask(UMASK);
//	test();
//	exit(0);
	config.userId = getuid();
	config.euserId = geteuid();
	initProgram(argc, argv);
	startSqlite();
	clearWorkDir();
	checkAndFix(false);
//	updateOwners();
//	exitProg(0);
	config.sqliteMode = "";
	loadDataFiles();	// load list of files to archive
	switch(config.cmd) {

	case RESTORE:
		restoreFromArchive();
		break;

	case SHOW:
		if(argv[1][0] == 'u')
			listUnrecover();
		else if(argv[1][0] == 'c')
			listChanged();
		break;

	case PLAN:
	case SYNC:
		prepareForSync();
		if(createSlices() < 0)
			break;
		if(config.cmd != PLAN) {
			config.cmd = SYNC;
			saveSlices();	// save new slices in backup dir
		}
		break;
	}
	exitProg(0);
}
