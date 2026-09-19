#ifndef BACKUP_H_
#define BACKUP_H_

#define	DEFAULT_TOLERANCE	1	// in percentage

#include "sqlite3.h"
#include "utils.h"
#include "sql.h"

int verbose(const char *fmt, ...);

#define DEFAULT_MAX_SLICE_FILES	1000
#define MAX_DISKS				10
#define FILE_SIZE_DIGITS		16
#define DISK_ID_SIZE			4
#define SLICE_PREFIX_LEN		4
#define SLICE_PREFIX			"slc-"
//#define INITIAL_PREFIX			"-"
#define SLICE_ID_VID_SIZE		4
#define VID_MASK				((1 << ((SLICE_ID_VID_SIZE - 1) * 4)) - 1)
#define SLICE_ID_SIZE			(SLICE_PREFIX_LEN + SLICE_ID_VID_SIZE + TIMESTAMP_SIZE)
#define HEADER_BUF_SIZE			(32 * 1024)
#define MIN_THE_SAME_SIZE		64	// bytes

#define MAX_FILES_PER_DISK		64
#define EOH						"EOH"
#define SYNC_START_MARKER		"SYNC_START_MARKER"
#define TEMP_SUBDIR				".parity_backup_temp/"
#define	DEFAULT_CONFIG_DIR		"/parity-backup/"
#define DEFAULT_BACKUP_SUBDIR 	".parity-backup/"
#define LAST_GENER				"list-slices.lst"
#define PARITY_DISK				(config.disks + 1)
#define PARITY_DIR				config.diskDesc[PARITY_DISK - 1].path
#define MARKER_LINK_SIZE		16
#define MAX_JOBS				32000	// Max rowids in job queue
#define VERBOSE(fmt, ...)		verbose(fmt, ##__VA_ARGS__)
#define LOCK_VERBOSE()			lockVerbose()
#define UNLOCK_VERBOSE()		unlockVerbose()
#define MAX_PATTERN_SIZE		20

enum COMMANDS {
	PLAN = 8, SYNC, RESTORE, SHOW	// if modified, change commNames array in backup.cpp
};

typedef struct {
	sqint		dseq;
	int			disk;
	char		hash[HASH_SIZE_DIGITS + 1];
} HASH_POS;

typedef struct {
	char		id[4];
	int			node_size;
	sqint		disk_size;
	sqint		free_size;
	sqint		filesSize;
	sqint		consumedSize;
	const char	*path;	// Full (absolute) path
	const char	*mountDir;
	__dev_t		f_sid;	// disk id
} DISK;	// Data or Parity directory


typedef struct {
	int				start;
	int				lastTime;
	JobQueue		*queue;
	pthread_mutex_t	mutex;			// guard for next data fields
	int				files2process;
	sqint			data2process;
	int 			filesProcessed;
	sqint			dataProcessed;
	sqint			lastProcessed;
	sqint			dataTick;
	int				filesSkipped;
	sqint			speedBytesStart;
	int				speedStartTime;
	bool 			operationCanceled;
	FILE			*raport;
} TCA;

typedef struct {
	const char		*configDir;
	const char		*workDir;
	const char		*configFile;
	int				disks;
	int				percentTolerance;
	int				maxThreads;
	int				maxFilesInSlice;
	int				maxSlices;				// max number of slices (for tests only)
	int				fileBufferSize;			// buffer size for file read/write
	char			*disk[MAX_DISKS];
	char			*contentsFile[MAX_DISKS];
	struct timeval	startTime;
	const char		*sqliteMode;
	bool 			restoreFlat;
	int 			replace;				// -1 not replace, 0 replace if zero length, 1 allow replace
	bool			acl;
	bool			allowSameDevices;
	bool			verbose;
	bool			verify;
//	bool			rebuild;
	bool			help;
	const char		*log;
	COMMANDS		cmd;
	sqint			progStart;		// data from watchdog table
	sqint			syncStart;		// -- || --
	sqint			lastVerifyDB;	// -- || --
	const char		**patterns;
	int				patternsCount;
	bool			patternIsPrefix;
	const char		*restoreDest;
	const char		*restoreDiskId;
	char			*excludePaths;
	char			*excludeNames;
	char			*excludeRegex;
	char			*options;
	char			*dryOption;
	regex_t			*regex[MAX_PATTERN_SIZE];
	int				restoreOwner;
	int				owner;
	int				userId;
	int				euserId;
	bool			restoreACLsupported;
	int				dry;	// <max size in MB>/<max-count>
	DISK			diskDesc[MAX_DISKS];
} CONFIG;

// Helper structures in memory for slices computing and save

typedef struct {
	sqint		fileSize;
	sqint		bytes2transfer;
	sqint		parityStart;	// offset in slice parity array
	char		mtime[TIMESTAMP_SIZE + 1];
	void		*hashState;
} SLICE_FILE;

// list of files from one disk
typedef struct {
	int64_t		sumFilesLen;	// in bytes
	int			filesInSlice;	// current number of files in slice
	bool 		diskFinished;
	int			fd;				// in file descriptor
	char		*dataBuf;
	int			datalen;		// length of data in buffer
	sqint		seq;			// id file in newfiles table
	off_t		fileOffset;		// for async read/write operation
	SLICE_FILE	file;
} SLICE_DISK;

typedef struct {
	int			maxFilledDisk;
	char		sliceId[SLICE_ID_SIZE + 1];
	bool		genCanceled;
	int			disk2write;
	char		markerName[MARKER_LINK_SIZE];
	sqint		writeStart;		// start of file in slice parity array
	sqint		writeSize;		// file size to write (parity or restore)
	char		savePath[PATH_MAX];
	SQL_COMMAND selectAll;
	SQL_COMMAND selectNext;
	sqint		fillFactor;
	sqint		sliceSize;	// sum files size (in bytes) for maxFilledDisk
	sqint		filesCount;
	uint		headerSize;	// offset to parity map
//	int			eohStart;	// offset to end of header start
	void 		*aio_cb;	// type of CB depends on AIO library. Posix, Linux AIO, io_uring ...
	HASH_POS	*hashArray;
	int			hashSize;
	int			hashCount;
	// For statusBar
	sqint		bytesWritten;
	int			filesWritten;
	int			filesCanceled;
	bool		outOfSpace;
	FILE		*raport;
	SLICE_DISK	disks[MAX_DISKS + 1];	// List of files in slice. Parity is last after config.disks
} SLICE;

int createSlices();
int loadDataFiles();
int getDiskPath(const char *path, char *buf = NULL, int bufSize = PATH_MAX);
void printSlice(const char *slid, sqint ssize, sqint files, const char *sstate, sqint fill);
int removeSlice(const char *sid, bool fileOnly = false);
void recalculateNewfiles();
bool findHash4seq(SLICE *cs, int diskIdx, sqint seq, char *hbuf);
void storeHash(SLICE *cs, int diskIdx, sqint seq = 0, const char *hash = NULL);
bool diskIsMounted(int diskIdx = -1);
bool isParity(int diskIdx);
SLICE_DISK *getParityDisk(SLICE *cs);
void printSliceState(SLICE *cs, const char *msg, int diskIdx = -1, int dumpSize = 0);
int openWorkFile(SLICE *cs, int diskIdx);
int saveWorkFile(SLICE *cs, int diskIdx);
const char *restoreDirs(const char *path, int *emul, bool withLast = false);
void disk2prefix(DISK *disk, char *path, bool removeNL = true);
void path2prefix(char *path, bool removeNL = true);
//void genACLfile(int didx);
void lockVerbose();
void unlockVerbose();
TCA *getTCA();
void intHandler(int signum);
void exitProg(int rv);
void copyConfig(bool confOnly = false, bool dryBackup = false);
int cancelGen(SLICE *cs, const char *fmt = NULL, ...);
FILE *getVerbose();
bool cancelRequest(SLICE *cs);
bool pathMatchPattern(const char *path, int owner = -1);
FILE *openTempFile(const char *pattern);
const char *getRestorePath();

#endif /* BACKUP_H_ */
