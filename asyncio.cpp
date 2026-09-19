#include "backup.h"
#include "asyncio.h"
#include <aio.h>

extern CONFIG config;

void initAIO() {
	struct aioinit ai;
	memset(&ai, 0, sizeof(ai));
	int threads = (config.maxThreads <= 1) ? 1 : config.maxThreads * config.disks + 2;
	ai.aio_threads = threads; // Maximum threads
	ai.aio_num = threads;     // Expected total simultaneous requests
	ai.aio_idle_time = 2;         // Seconds to keep idle threads alive
	aio_init(&ai);
}

static bool involved(SLICE *cs, sqint start, sqint size) {
	return (cs->writeStart <= start + size && start <= cs->writeStart + cs->writeSize);
}

// Is finished if closed or open and zero bytes to transfer
static bool fileFinished(SLICE *cs, int diskIdx) {
	SLICE_DISK *sd = cs->disks + diskIdx;
	if(sd->file.bytes2transfer == 0) {
		if(sd->fd <= 0 || sd->file.fileSize == 0)
			return true;
		if(sd->fd > 0)
			if(sd->file.parityStart + sd->file.fileSize <= cs->writeStart + cs->writeSize || sd->fileOffset == sd->file.fileSize)
				return true;
	}
	return false;
}

static void fitTransferOption(SLICE *cs, int diskIdx) {
	SLICE_FILE *cf = &cs->disks[diskIdx].file;
	sqint ext = cf->parityStart + cf->bytes2transfer - (cs->writeStart + cs->writeSize);
	if(ext > 0)
		cf->bytes2transfer -= ext;
	if(cf->parityStart < cs->writeStart) {
		SLICE_DISK *sd = cs->disks + diskIdx;
		sd->fileOffset = cs->writeStart - cf->parityStart;
		cf->bytes2transfer -= sd->fileOffset;
	}
}

static int newTargetFile(SLICE *cs) {
	for(int i = 0; i < PARITY_DISK; i++) {
		if(i != cs->disk2write) {
			SLICE_DISK *sd = cs->disks + i;
			sd->diskFinished = false;
			SLICE_FILE *cf = &cs->disks[i].file;
			if(involved(cs, cf->parityStart, cf->fileSize)) {
				cf->bytes2transfer = cf->fileSize;
				sd->fileOffset = 0;
				fitTransferOption(cs, i);
			} else {
				if(sd->fd > 0) {
					close(sd->fd);
					sd->fd = -1;
				}
			}
		}
	}
	return 0;
}

// Return destination disk path and path to file on disk
static const char *getRestoreDest(const char *virtPath, const char **filePath) {
	const char *target = config.restoreDest;
	if(target == NULL)
		target = config.diskDesc[getDiskIndex(virtPath)].path;
	if(filePath) {
		const char *dirStart = strchr(virtPath, getPathSeparator());
		if(dirStart)
			dirStart++;
		*filePath = dirStart;
	}
	return target;
}

// Load file description from database. Always try to read one record
static char *getNextFile(SLICE *cs, int diskIdx) {
	SQL_COMMAND *slice = &cs->selectNext;
	SLICE_DISK *sd = cs->disks + diskIdx;
	SLICE_FILE *cf = &sd->file;
	char *diskId = config.diskDesc[diskIdx].id;
	const void *bindVar[] = {cs->sliceId, diskId, &sd->seq};
	sqint dseq, fsize, start, realSize;
	char *pstate, *path, *mtime, realModify[TIMESTAMP_SIZE + 1];
	int st = 0;
	bool sync = isParity(cs->disk2write);
	const void *vars[] = {&dseq, &fsize, NULL, &start, NULL, NULL};
	// step until file involved in sync/restore
	while(true) {
		SQL(slice, bind, bindVar);
		st = SQL(slice, step, vars);
		path = (char *) vars[2];
		mtime = (char *) vars[4];
		sd->seq = dseq;
		if(st <= 0 || sync)
			break;	// SYNC, open next, no filter
		if(cs->disk2write == diskIdx) {
			if(pathMatchPattern(path, config.restoreOwner)) {
				// File for restore. Check if file exists
				COPY_FLD(cf->mtime, mtime);
				if(config.restoreFlat)
					break;
				pstate = (char *) vars[5];
				// Check if file should be restored
				const char *filePath, *diskPath = getRestoreDest(path, &filePath);
				realSize = fileInfo(diskPath, filePath, realModify);
				if((pstate[0] == '*' || fsize != realSize || strcmp(mtime, realModify)) && (config.replace > 0 || realSize <= config.replace))
					break;
				// skip file
				cs->filesCanceled++;
				cs->filesWritten++;
				cs->bytesWritten += fsize;
			}
		} else if(start + fsize >= cs->writeStart)
			break;
	}
	if(st <= 0) {
		sd->diskFinished = true;
		path = NULL;
	} else {
		if(cf->fileSize && fsize > cf->fileSize) {
			cancelGen(cs, "Improper file size in sequence (should be descending). File: %s", path);
			return NULL;
		}
		cf->fileSize = fsize;
		cf->parityStart = start;
		if(diskIdx == cs->disk2write)
			strcpy(cs->savePath, path);
	}
	return path;
}

static void finishCurrentFile(SLICE *cs, int diskIdx) {
	// Data file EOF.
	SLICE_DISK *sd = cs->disks + diskIdx;
	VERBOSE("Finish file. Disk: %d\n", diskIdx);
	if(config.cmd == RESTORE) {
		if(diskIdx == cs->disk2write) {
			// compare computed hash with this stored in slice
			char hash[HASH_SIZE_DIGITS + 1] = {0}, chash[HASH_SIZE_DIGITS + 1] = {0};
			SLICE_FILE *cf = &sd->file;
			VERBOSE("Compare hash values\n");
			if(cf->fileSize > 0 && findHash4seq(cs, diskIdx, sd->seq, hash)) {
				getBlockHash(cf->hashState, chash);
				if(strcmp(hash, chash) != 0) {
					cancelGen(cs, "Different hash values ");
				}
			}
			saveWorkFile(cs, diskIdx);
			if(!cs->genCanceled)
				cs->filesWritten++;
			cf->parityStart = cf->fileSize = 0;
		}
	} else if( ! isParity(diskIdx)) {	// SYNC
		VERBOSE("Store hash (size: %lu)\n", getHashStateLen(sd->file.hashState));
		storeHash(cs, diskIdx);
	}
}

static char *openNextFile(SLICE *cs, int diskIdx) {
	SLICE_DISK *sd = cs->disks + diskIdx, *parDisk = getParityDisk(cs);
	SLICE_FILE *cf = &sd->file;
	if(cs->genCanceled || isParity(diskIdx) || sd->diskFinished)
		return NULL;
	if(! fileFinished(cs, diskIdx))
		return NULL;
	if(sd->fd > 0) {
		finishCurrentFile(cs, diskIdx);
		close(sd->fd);
		sd->fd = -1;
	}
	char *path = getNextFile(cs, diskIdx);
	if(path) {
		// next file desc loaded from database
		const char *fdir = config.diskDesc[diskIdx].path;
		char *diskId = config.diskDesc[diskIdx].id;
		bool sync = isParity(cs->disk2write);
		cf->bytes2transfer = cf->fileSize;
		sd->fileOffset = 0;
		if(sync){
			sd->filesInSlice += 1;
			startBlockHash(cf->hashState);
			VERBOSE("Start hash. Disk: %d  Size: %lu\n", diskIdx, getHashStateLen(cf->hashState));
		} else {
			// RESTORE
			if(cs->disk2write == diskIdx) {
				if(openWorkFile(cs, diskIdx) < 0)
					return NULL;
				// Set start offset in parity file
				parDisk->fileOffset = cs->writeStart = cf->parityStart;
				cs->writeSize = cf->fileSize;
				cs->genCanceled = false;
				startBlockHash(cf->hashState);
				VERBOSE("Start hash. Disk: %d  Size: %lu\n", diskIdx, getHashStateLen(cf->hashState));
			} else {
				fitTransferOption(cs, diskIdx);
			}
		}
		if(sd->fd <= 0) {
			path += strlen(diskId) + 1;
			int readMode = (cs->disk2write != diskIdx) ? O_RDONLY : O_WRONLY | O_CREAT;
			sd->fd = openAt(path, readMode, fdir);
			if(sd->fd < 0) {
				if(cs->disk2write != diskIdx)
					cancelGen(cs, "Can't open file %s%s", fdir, path);
				else
					cancelGen(cs);
				path = NULL;
			} else {
				posix_fadvise(sd->fd, 0, 0, POSIX_FADV_NOREUSE);
			}
		}
	}
	printSliceState(cs, "Open next", diskIdx);
	return path;
}

void startAIO(SLICE *cs, int diskIdx) {
	aiocb *cb = ((aiocb *) cs->aio_cb) + diskIdx;
	bzero(cb, sizeof(aiocb));
	SLICE_DISK *sd = cs->disks + diskIdx;
	if(cs->genCanceled || sd->diskFinished)
		return;
	SLICE_FILE *sf = &sd->file;
	int readMode = (cs->disk2write != diskIdx);
	if(readMode && sf->bytes2transfer <= 0 && openNextFile(cs, diskIdx) == NULL) // input data disk. Open next file?
		return;
	if(readMode) {
		if(sd->datalen >= config.fileBufferSize || sf->bytes2transfer <= 0)
			return;	// Buffer full or nothing to read
	} else if(sd->datalen == 0) {
		return;	// nothing to write
	}
	const char *msg = "read";
	int st;
	cb->aio_fildes = sd->fd;
	cb->aio_offset = sd->fileOffset;
	if(readMode) {
		int freeSpace = config.fileBufferSize - sd->datalen;
		off_t rest = sf->bytes2transfer;
		if(isParity(diskIdx)) {
			rest = cs->writeSize - (sd->fileOffset - cs->writeStart);
			cb->aio_offset += cs->headerSize;
		}
		rest = MIN(rest, freeSpace);
		cb->aio_nbytes = rest;	// rest of file
		cb->aio_buf = sd->dataBuf + sd->datalen;
		st = aio_read(cb);
	} else {
		cb->aio_nbytes = sd->datalen;
		cb->aio_buf = sd->dataBuf;
		if(!config.dry)
			st = aio_write(cb);
		else
			st = 0;
		msg = "write";
	}
	if(st < 0) {
		char path[PATH_MAX];
		bzero(cb, sizeof(aiocb));
		cancelGen(cs, "AIO init %s failed %s", msg, getFilePath(sd->fd, path));
		return;
	}
	printSliceState(cs, "AIO start", diskIdx);
}

static int suspend(SLICE *cs) {
	bool wait = false, rv = false;
	aiocb *wcb[PARITY_DISK] = {0};
	for(int i = 0; i < PARITY_DISK; i++) {
		aiocb *cb = ((aiocb *) cs->aio_cb) + i;
		if(cb->aio_nbytes > 0) {
			rv = true;
			if(config.dry && i == cs->disk2write)
				continue;
			wcb[i] = cb;
			wait = true;
		}
	}
	if(wait)
		aio_suspend(wcb, PARITY_DISK, NULL);
	return rv;
}

static bool cancelAIO(SLICE *cs, aiocb *cb) {
	bool canBreak = true;
	if(config.dry) {
		aiocb *cb2 =  ((aiocb *) cs->aio_cb) + cs->disk2write;
		bzero(cb2, sizeof(aiocb));
	}
	if(cb && cb->aio_nbytes > 0) {
		char path[PATH_MAX];
		log_error("AIO init failed %s", getFilePath(cb->aio_fildes, path));
		bzero(cb, sizeof(aiocb));
	}
	for(int i = 0; i < PARITY_DISK; i++) {
		cb =  ((aiocb *) cs->aio_cb) + i;
		if(cb->aio_nbytes > 0) {
			int st = aio_cancel(cb->aio_fildes, cb);
			if(st == AIO_NOTCANCELED)
				canBreak = false;
			else
				bzero(cb, sizeof(aiocb));
		}
	}
	return canBreak;
}

// Read data into slice disks buffers and write parity stream to slice output file
static int wait4buffers(SLICE *cs) {
	SLICE_DISK *sd;
	aiocb *cb = NULL;
	int rv = 0, aioStat;
	do {
		cancelRequest(cs);
		// Open target file (parity file or file to restore)
		sd = cs->disks + cs->disk2write;
		if(sd->fd <= 0) {
			while(sd->file.fileSize == 0) {
				if(openNextFile(cs, cs->disk2write) == NULL)
					return -1;
				newTargetFile(cs);
				if(sd->file.fileSize == 0)
					finishCurrentFile(cs, cs->disk2write);
			}
		}
		for(int i = 0; i < PARITY_DISK; i++) {
			sd = cs->disks + i;
			cb =  ((aiocb *) cs->aio_cb) + i;
			if(sd->diskFinished)
				continue;	// all files on this disk processed
			if(cb->aio_nbytes == 0) {	// not initiated
				if(cs->genCanceled)
					continue;	// continue check for started and not finished operations
				// try to init next operation
				startAIO(cs, i);
				if(cs->genCanceled)
					break;
				continue;
			}
			// operation initiated (aio_nbytes > 0)
			ssize_t len;
			if(i == cs->disk2write && config.dry && cb->aio_nbytes > 0) {
				len = cb->aio_nbytes; // simulate write
			} else {
				aioStat = aio_error(cb);
				if(aioStat == EINPROGRESS)
					continue;
				len = aio_return(cb);
			}
			if(len < 0) {
				char p[PATH_MAX];
				rv = cancelGen(cs, "AIO failed on file %s", getFilePath(cb->aio_fildes, p));
				break;
			}
			SLICE_FILE *cf = &sd->file;
			if(i != cs->disk2write) {
				// files on disks to read
				// consume read and continue reading
				if(len > 0) {
					if(isParity(cs->disk2write)) {
						// Not restore.
						VERBOSE("Update hash. Disk %d  %lu + %d bytes\n", i, getHashStateLen(cf->hashState), len);
						if(updateBlockHash(cf->hashState, sd->dataBuf + sd->datalen, len) < 0) {
							char path[PATH_MAX];
							log_fatal("Hash update block error for file %s", getFilePath(sd->fd, path));
						}
					}
					sd->datalen += len;
					sd->sumFilesLen += len;
					sd->fileOffset += len;
					sd->file.bytes2transfer -= len;
				} else { // len == 0, EOF on file
					if(cf->bytes2transfer != 0) {
						char path[PATH_MAX];
						cancelGen(cs, "Size of file changed. [%s]", getFilePath(sd->fd, path));
						cf->bytes2transfer = 0;
						bzero(cb, sizeof(aiocb));
						break;
					}
				}
				startAIO(cs, i);
			} else {
				// disk to write/parity disk
				if(len != sd->datalen) {
					cancelGen(cs, "AIO not write whole data to file. %ld <> %ld", len, sd->datalen);
					break;
				}
				// Clean after write
				if( ! isParity(i)) {
					VERBOSE("Update hash. Disk %d  %lu + %d bytes\n", i, getHashStateLen(cf->hashState), len);
					if(updateBlockHash(cf->hashState, sd->dataBuf, sd->datalen) < 0) {
						char path[PATH_MAX];
						log_fatal("Hash update block error for file", getFilePath(sd->fd, path));
					}
				}
				bzero(sd->dataBuf, sd->datalen);
				sd->datalen = 0;
				sd->fileOffset += len;
				cf->bytes2transfer -= len;
				cs->bytesWritten += len;
				bzero(cb, sizeof(aiocb));
				if(cf->bytes2transfer <= 0)
					finishCurrentFile(cs, i);
			}
		}
		printSliceState(cs, "wait4buffer");
		if(cs->genCanceled && cancelAIO(cs, cb)) {
			if(isParity(cs->disk2write) || cs->outOfSpace)
				break;
			// Try restore next file from slice
			cs->filesCanceled++;
			cs->filesWritten++;
			sd = cs->disks + cs->disk2write;
			cs->bytesWritten += sd->file.bytes2transfer;
			sd->file.bytes2transfer = 0;
			if(sd->fd > 0) {
				close(sd->fd);
				sd->fd = -1;
			}
		}
	} while(suspend(cs));
	return rv;
}

// Control access to disk. Asynchronously read/write data and parity streams.
int fillInputBuffers(SLICE *cs) {
	// AIO control blocks for slice data files and slice result file
	aiocb lcb[PARITY_DISK] = {0};
	cs->aio_cb = lcb;
	int rv = wait4buffers(cs);
	cs->aio_cb = NULL;
	return rv;
}
