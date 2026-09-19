#include "backup.h"
#include "os.h"

extern CONFIG config;
//extern bool showStatusBar;
#define BALANCED_DISK_FILL_LIMIT	95

static int maxSizeDisk;
static SLICE *cSlice = NULL;
static const char *NOT_OWNER_LIST = "notOwner.lst";

SQL_COMMAND insert = {.cmd =
	"INSERT INTO slpos (sid, disk, dseq, fsize, start, mtime, fpath, owner) VALUES (?, ?, :i1, :i2, :i3, ?, ?, ?)"};

SQL_COMMAND sel = {.cmd =
	" SELECT disk, seq, size, mtime, name, owner FROM newfiles WHERE"
	" size >= 0 AND state = '' AND type == 'f' AND disk = ? AND seq BETWEEN :i1 AND :i2 ORDER BY seq"};

SQL_COMMAND update = {.cmd =
	"UPDATE newfiles SET state = '*' WHERE size >= 0 AND state = '' AND type == 'f' AND disk = ? AND seq between :i1 AND :i2"};

SQL_COMMAND add = {.cmd =
	"SELECT seq, size, disk FROM newfiles WHERE disk = ? AND state = '' AND type == 'f' AND size >= 0 AND size <= :i ORDER BY seq"};

SQL_COMMAND head = {.cmd =
	"INSERT INTO slhead (sid, state) VALUES (?, ?)"};

SQL_COMMAND insLink = {.cmd = "INSERT INTO links (ftype, link_name, target) VALUES (?, ?, ?)"};

// Check if sizes are near the same (with percent tolerance)
int percentDiff(sqint size1, sqint size2) {
	sqint diff = abs(size1 - size2);
	if(diff <= MIN_THE_SAME_SIZE)	// differences that are treated as the same
		return 0;
	double p = (double(diff) * 100.0) / double(MAX(size1, size2));
	int prc = int(round(p));
	if(prc <= config.percentTolerance)
		return 0;
	if(size1 < size2)
		return -prc;
	return prc;
}

void printSlice(const char *slid, sqint ssize, sqint files, const char *sstate, sqint fill) {
	if(getVerbose()) {
		char cmd[256], fs[128], *path, *state, ts[TIMESTAMP_SIZE_V + 1];
		sqint fsize;
		sprintf(fs, "%ld", ssize);
		int col1 = strlen(fs);
		if(col1 < 9)
			col1 = 9;
		LOCK_VERBOSE();
		VERBOSE("\nSlice: %s    State: %s   Fill: %ld%%\n", slid, sstate, fill);
		VERBOSE("%*s  %-*sPath\n", col1, "File size", TIMESTAMP_SIZE_V + 1, "Modify time");
		VERBOSE(fs, '-', TIMESTAMP_SIZE_V + 1);
		VERBOSE("%.*s ", col1, fs);
		VERBOSE(" %.*s %.4s\n", TIMESTAMP_SIZE_V, fs, fs);
		sprintf(cmd, "SELECT fsize, mtime, fpath, state FROM slpos WHERE sid = '%s' ORDER BY disk, dseq", slid);
		SQL_COMMAND pos = {.cmd = cmd};
		const void *dvars[] = {&fsize, NULL, NULL, NULL};
		while(SQL(&pos, step, dvars) > 0) {
			path = (char *) dvars[2];
			state = (char *) dvars[3];
			timestamp2str((char *) dvars[1], ts, sizeof(ts));
			VERBOSE("%*ld %1s%*s %s\n", col1, fsize, state, TIMESTAMP_SIZE_V, ts, path);
		}
		VERBOSE("%*ld  Files: %ld\n", col1, ssize, files);
		UNLOCK_VERBOSE();
		SQL(&pos, finalize);
	}
}

int fetchNextSize(char *diskId, sqint *seq, sqint *fsize) {
	sqint maxSize = LONG_MAX;
	const void *bindVars[2] = {diskId, &maxSize};
	SQL(&add, bind, bindVars);
	const void *dataVars[3] = {seq, fsize, NULL};
	return SQL(&add, step, dataVars);
}

static sqint diskInUse(int diskIdx) {
	if(config.maxFilesInSlice && cSlice->disks[diskIdx].filesInSlice >= config.maxFilesInSlice)
		return 0;
	sqint rv = config.diskDesc[diskIdx].filesSize - config.diskDesc[diskIdx].consumedSize;
	return MAX(rv, 0);
}

static sqint computeFillFactor(sqint dsizes[] = NULL) {
	sqint fillFactor = 0, slSize = 0, val;
	for(int i = 0; i < config.disks; i++) {
		SLICE_DISK *sdisk = cSlice->disks + i;
		val = dsizes ? dsizes[i] : sdisk->sumFilesLen;
		fillFactor += val;
		slSize = MAX(val, slSize);
	}
	if(slSize == 0)
		return -1;
	double p = (fillFactor * 100.0) / (config.disks * slSize);
	fillFactor = (sqint) round(p);
	return fillFactor;
}

// Find disk to fill (biggest difference between fill)
int selectNextDisk() {
	SLICE_DISK *sdisk;
	sqint msize = cSlice->disks[cSlice->maxFilledDisk].sumFilesLen;
	int maxPercent = 0, nextDisk = -1, maxSizePercent = 100;
	for(int i = 0; i < config.disks; i++) {
		sdisk = cSlice->disks + i;
		if(diskInUse(i) && !sdisk->diskFinished) {
			int diff = percentDiff(sdisk->sumFilesLen, msize);
			if(diff < maxPercent) {
				nextDisk = i;
				maxPercent = diff;
			}
			if(i == maxSizeDisk)
				maxSizePercent = abs(diff);
		}
	}
	if(abs(maxPercent) <= config.percentTolerance)
		return -1;	// slice ready
	if(cSlice->maxFilledDisk != maxSizeDisk && maxSizePercent > config.percentTolerance) {
		sdisk = cSlice->disks + maxSizeDisk;
		if(diskInUse(maxSizeDisk) && !sdisk->diskFinished)
			return maxSizeDisk;
	}
	return nextDisk;
}

int freshSliceStat(int diskIdx, int fcount, sqint fsize) {
	SLICE_DISK *sdisk = cSlice->disks + diskIdx;
	sdisk->filesInSlice += fcount;
	cSlice->filesCount += fcount;
	sdisk->sumFilesLen += fsize;
	if(sdisk->sumFilesLen > cSlice->sliceSize) {
		cSlice->sliceSize = sdisk->sumFilesLen;
		cSlice->maxFilledDisk = diskIdx;
	}
	return sdisk->filesInSlice;
}

void clearSliceStat(SLICE *cs) {
	// Clear structures to start next
	cs->sliceSize = cs->maxFilledDisk = cs->filesCount = 0;
	for(int i = 0; i < config.disks; i++) {
		SLICE_DISK *sd = cs->disks + i;
		DISK *dsk = config.diskDesc + i;
		dsk->consumedSize += sd->sumFilesLen;
		if(dsk->consumedSize > dsk->filesSize)
			log_fatal("Program BUG. Sum of consumed files bigger than sum of files for disk: %s", dsk->id);
		sd->filesInSlice = sd->sumFilesLen = 0;
		sd->diskFinished = false;
	}
}

bool insert2slice(int diskIdx, sqint startSeq, sqint lastSeq, sqint fcount, sqint fsum) {
	bool rest = false;
	if(startSeq == 0 && lastSeq == 0) {
		startSeq = 1;
		lastSeq = LONG_MAX;
		rest = true;
	}
	if(getVerbose()) {
		char range[64] = "rest";
		if(lastSeq != LONG_MAX)
			snprintf(range, sizeof(range), "%ld, %ld", startSeq, lastSeq);
		VERBOSE("Append %ld files [ %s ] from disk: %s  files size: %s (%ld)\n",
				fcount,	range, config.diskDesc[diskIdx].id, formatFileSize(fsum), fsum);
	}
	sqint fsize, seq, soffset = cSlice->disks[diskIdx].sumFilesLen, filesAdded = 0, bytesAdded = 0;
	const void *bindVars[] = {config.diskDesc[diskIdx].id, &startSeq, &lastSeq};
	SQL(&sel, bind, bindVars);
	const void *dvars[] = {NULL, &seq, &fsize, NULL, NULL, NULL};
	const void *insVal[] = {cSlice->sliceId, config.diskDesc[diskIdx].id, &seq, &fsize, &soffset, NULL, NULL, NULL};
	while(SQL(&sel, step, dvars) > 0) {
		insVal[5] = dvars[3];	// modify time
		insVal[6] = dvars[4];	// path
		insVal[7] = dvars[5];	// owner
		SQL(&insert, bind, insVal);
		SQL(&insert, step);
		soffset += fsize;
		filesAdded++;
		bytesAdded += fsize;
		if(rest && (bytesAdded > (2 * long(GIGA)) || filesAdded > config.maxFilesInSlice)) // limit ~2 GB
			break;
	}
	// update status
	if(!rest && (filesAdded != fcount || bytesAdded != fsum))
		log_fatal("Incorrect sum %ld != %ld or %ld != %ld", filesAdded, fcount, bytesAdded, fsum);
	int files = freshSliceStat(diskIdx, filesAdded, bytesAdded);
	lastSeq = seq;
	SQL(&update, bind, bindVars);
	SQL(&update, step);
	return (files >= config.maxFilesInSlice);
}

sqint initSliceFiles(int diskIdx, sqint maxSize) {
	sqint seq = 0, fsize = 0, rv = 0;
	int st = fetchNextSize(config.diskDesc[diskIdx].id, &seq, &fsize);
	if(st > 0) {
		insert2slice(diskIdx, seq, seq, 1, fsize);
		rv = fsize;
	}
	return rv;
}

int startSlice(int slcCount) {
	// create sliceId. slc-vid-timestamp
	slcCount &= VID_MASK;
	sprintf(cSlice->sliceId + SLICE_PREFIX_LEN, "%0*x-", SLICE_ID_VID_SIZE - 1, slcCount);
	getTimestamp(cSlice->sliceId + SLICE_PREFIX_LEN + SLICE_ID_VID_SIZE);
	VERBOSE("\n------Init slice: %s\n", cSlice->sliceId);
	int activeDisks = 0, first = 0;
	for(int i = 0; i < config.disks; i++) {
		if(diskInUse(i) && ! cSlice->disks[i].diskFinished)
			if(initSliceFiles(i, 0) > 0) {
				activeDisks++;
				first = i;
			}
	}
	if(activeDisks == 1)
		return first;
	return -1;
}

void fillHole(int diskIdx, sqint holeSize) {
	sqint startRowid = 0, lastRowid = 0, sizeSum = 0, fileCount = 0;
	sqint seq, fileSize;
	SLICE_DISK *sd = cSlice->disks + diskIdx;
	const void *bindVars[2] = {config.diskDesc[diskIdx].id, &holeSize};
	const void *dataVars[3] = {&seq, &fileSize, NULL};
	int st, first, diskFiles = cSlice->disks[diskIdx].filesInSlice;
//	sqint oldHoleSize = holeSize, hcount = 0;
	while(holeSize > 0) {
		SQL(&add, bind, bindVars);
		first = true;
		while (true) {
			// iterate on file sizes descendig
			seq = 0;
			if(config.maxFilesInSlice && diskFiles + fileCount >= config.maxFilesInSlice) {
				holeSize = 0;
				break;
			}
			st = SQL(&add, step, dataVars);
			if(st > 0) {
				// file found
				if(percentDiff(fileSize, holeSize) == 0) {
					// file completely fill hole
					holeSize = 0;	// break on outer loop
					break;			// break inner loop
				} else {
					// not fit
					if(holeSize < fileSize) {
						// to big
						if(diskIdx != maxSizeDisk || !first)
							// Not accepted, can't be bigger
							seq = 0;
						first = false;
						break;
					}
					// decrease hole size and continue
				}
				first = false;
				if(startRowid == 0)
					startRowid = seq;
			} else {
				if(first) {
					sd->diskFinished = true;
					holeSize = 0;
				}
				// reselect with smaller hole size
				break;
			}
			if(seq) {
				fileCount++;
				lastRowid = seq;
				sizeSum += fileSize;
				holeSize -= fileSize;
			}
		}
		if(seq) {
			fileCount++;
			lastRowid = seq;
			sizeSum += fileSize;
		}
		if(startRowid == 0)
			startRowid = lastRowid;
		// save continous list of files and eventually continue if the hole still exists
		if(startRowid && lastRowid) {
			if(insert2slice(diskIdx, startRowid, lastRowid, fileCount, sizeSum))
				holeSize = 0;	// slice too big. Break
			else
				diskFiles = cSlice->disks[diskIdx].filesInSlice;
			startRowid = lastRowid = sizeSum = fileCount = 0;
		}
	}
	// hole filled
}

int addFile2slice() {
	int nextDisk = selectNextDisk();
	if(nextDisk >= 0) {
		sqint maxSize = cSlice->disks[cSlice->maxFilledDisk].sumFilesLen - cSlice->disks[nextDisk].sumFilesLen;
		fillHole(nextDisk, maxSize);
	}
	return nextDisk;
}

int newfileIndex(bool create) {
	if(create) {
		char cmd[1024] =
			"CREATE INDEX newsize on newfiles (state, disk, seq desc)";
		if(SQL_EXEC(cmd) < 0)
			log_fatal("Error creating indexes on 'newfiles' table");
	} else {
		SQL_EXEC("DROP INDEX IF EXISTS newsize");
	}
	return 0;
}

static void saveSpecialFile(const char *path, FILE *lnFile, const char ftype) {
	char lpath[strlen(path) + 16];
	strcpy(lpath, path);
	if(ftype == 'l') {
		char link[PATH_MAX], *p = realpath(path, link);
		path2prefix(lpath, false);
		if(p) {
			path2prefix(link, false);
			fprintf(lnFile, "%c %s\n%s\n", ftype, lpath, link);
		}
		return;
	} else if(ftype == 'h') {
		char *tok[2], stype[2] = {0};
		split(lpath, tok, 2, BLANKS);
		path2prefix(tok[1], false);
		fprintf(lnFile, "%c %s\n%s\n", ftype, tok[0], tok[1]);
		const void *dvars[] = {stype, tok[0], tok[1]};
		stype[0] = ftype;
		// temporary load
		SQL(&insLink, bind, dvars);
		SQL(&insLink, step);
		return;
	}
	// empty dir
	path2prefix(lpath, false);
//	addPathSeparator(lpath);
	fprintf(lnFile, "%c %s\n", ftype, lpath);
}

int loadLinks(int diskIdx) {
	char sline[PATH_MAX], dline[PATH_MAX], *ftype = sline;
	const void *dvars[] = {ftype, sline + 2, dline};
	DISK *disk = config.diskDesc + diskIdx;
	sprintf(sline, "DELETE FROM links WHERE link_name LIKE 'D%d%%'", diskIdx + 1);
	SQL_EXEC(sline);
	sprintf(sline, "%s%s.lnk", config.workDir, disk->id);
	if(fileInfo(NULL, sline, NULL) <= 0)
		return 0;	// No symlink in file
	FILE *symFile = fopen(sline, "r");
	if(symFile == NULL)
		log_fatal("Unable read file list '%s'", sline);
	SQL(&insLink, prepare);
	BEGIN();
	while(fgets(sline, sizeof(sline), symFile) != NULL) {
		removeLastNewline(sline);
		sline[1] = 0;
		if(ftype[0] == 'l' || ftype[0] == 'h') {
			if(fgets(dline, sizeof(dline), symFile) == NULL)
				log_fatal("No target specified for link: %s", sline);
			removeLastNewline(dline);
		} else
			dline[0] = 0;
		SQL(&insLink, bind, dvars);
		SQL(&insLink, step);
	}
	COMMIT();
	SQL(&insLink, finalize);
	fclose(symFile);
	return 0;
}

int saveEmptyDirs(const char *diskId, FILE *symFile) {
	char cmd[256], cmd2[256], type;
	const char *nm, *nm2;
	sprintf(cmd, "SELECT name, type FROM newfiles WHERE disk = '%s' AND type = 'd'", diskId);
	sprintf(cmd2, "SELECT name FROM newfiles WHERE disk = '%s' AND (type = 'l' OR type = 'f') AND name > ? ORDER BY name LIMIT 1", diskId);
	SQL_COMMAND seld = {.cmd = cmd};
	SQL_COMMAND seln = {.cmd = cmd2};
	const void *dvars[] = {NULL, NULL};
	const void *dvars2[] = {NULL};
	const void *bvars[] = {NULL};
	int n, k = 0;
	while(SQL(&seld, step, dvars) > 0) {
		bvars[0] = nm = (char *) dvars[0];
		type = ((char *) dvars[1])[0];
		// empty directory?
		dvars2[0] = NULL;
		k++;
		SQL(&seln, bind, bvars);
		n = SQL(&seln, step, dvars2);
		nm2 = (char *) dvars2[0];
		if(n > 0 && isPrefix(nm, nm2))
			continue;	// file in directory, skip
		// empty dir, save
		saveSpecialFile(nm, symFile, type);
	}
	SQL(&seld, finalize);
	if(k)
		SQL(&seln, finalize);
	return 0;
}

int loadFileList(int diskIdx, FILE *ownerList) {
	char line[PATH_MAX + 100], *tok[7], *p, mtime[24], *owner, *fpath, ftype, lastPath[PATH_MAX];
	const char *ownerName;
	FILE *f = NULL, *symFile = NULL, *aclList = NULL;
	DISK *disk = config.diskDesc + diskIdx;
	char *diskId = disk->id;
	sprintf(line, "%s%s.files", config.workDir, diskId);
	generateFileList(diskIdx, line);
	f = fopen(line, "r");
	if(f == NULL)
		log_fatal("Unable open data file: %s", disk->path);
	if(fileInfo(f) == 0)
		log_fatal("List not created.");
	if(config.cmd == SYNC) {
		sprintf(line, "%s%sacl.list", config.workDir, diskId);
		unlink(line);
		aclList = fopen(line, "w");
		sprintf(line, "%s%s.lnk", config.workDir, diskId);
		unlink(line);
		symFile = fopen(line, "w");
		if(symFile == NULL || aclList == NULL)
			log_fatal("Unable write to .lnk or .acl file list");
	}
	const void *bvar[] = {diskId};
	SQL_EXEC("DELETE FROM newfiles WHERE disk = ?", NULL, bvar);
	sqint file_size, inode, autoInc = 0, lastInode = 0;
	SQL_COMMAND insertf = {.cmd =
		"INSERT INTO newfiles (size, type, mtime, disk, name, seq, owner, inode) VALUES (:i1, ?, ?, ?, ?, :i2, ?, :i3)"};
	int prefixLen = strlen(disk->path), k, len;
	int uid = config.euserId, notOwner = 0, fowner;
	BEGIN();
	while(fgets(line, sizeof(line), f) != NULL) {
		k = split(line, tok, 7, BLANKS, 6);
		if(k > 4) {
			owner = tok[3];
			fpath = tok[5];
			ftype = tok[1][0];
			file_size = atoll(tok[0]);
			inode = atoll(tok[4]);
			removeLastNewline(fpath);
			if(fpath[prefixLen] == 0)
				continue;	// disk mount point, skip
			fowner = atoi(owner);
			if(euidaccess(fpath, R_OK)) {
				log_info("Skip not readable path '%s'", fpath);
				continue;
			}
			if(uid != 0 && fowner != uid) {
				notOwner++;
				if(notOwner <= 3)
					log_info("You are not owner (%s) of the file: %s", owner, fpath);
				findUser(owner, &ownerName);
				fprintf(ownerList, "%-11s %10ld %3c  %s\n", ownerName, file_size, ftype, fpath);
			}
			if(aclList) {
				if(lastInode == inode) {	// hard link
					disk2prefix(disk, fpath);
					fprintf(symFile, "%c %s\n%s\n", 'h', fpath, lastPath);
					continue;
				}
				char p4[PATH_MAX];
				strcpy(p4, fpath);
				path4shell(p4, sizeof(p4));
				fprintf(aclList, "%s\n", p4);
				if(ftype == 'l') {
					saveSpecialFile(fpath, symFile, ftype);
					continue;
				}
			}
			disk2prefix(disk, fpath);
			lastInode = inode;
			strcpy(lastPath, fpath);
			if(ftype == 'f')
				disk->filesSize += file_size;
			// modify timestamp
			p = strchr(tok[2], '.');
			if(p) {
				// Linux 'find' show nanoseconds on 10 positions instead 9
				len = strlen(p);
				if(len > 11 || (len > 10 && p[10] != '0'))
					log_fatal("Improper file modify time format: %s", tok[2]);
				if(len > 10)
					p[10] = 0;
				len = p - tok[2];
				if(len < 10) {
					k = sprintf(mtime, "%10.10ld", strtol(tok[2], NULL, 0));
					strcpy(mtime + k, p);
				} else
					strcpy(mtime, tok[2]);
			}
			else
				log_fatal("Improper file modify time format: %s", tok[2]);
			autoInc++;
			const void *bindVars[] = {&file_size, tok[1], mtime, diskId, fpath, &autoInc, owner, &inode};
			SQL(&insertf, bind, bindVars);
			SQL(&insertf, step, NULL);
		}
	}
	COMMIT();
	SQL(&insertf, finalize);
	if(aclList) {
		// append empty dirs to symlink file
		fclose(aclList);
		genACLfile(diskIdx);
		saveEmptyDirs(diskId, symFile);
//		saveHardLinks(diskIdx, symFile);
		fclose(symFile);
	}
	formatSize(disk->filesSize, line, false);
	log_info("Load %d files description. Cumulative size: %s", autoInc, line);
	fclose(f);
	return notOwner;
}

static void refreshState(int diskIdx, sqint *removed, sqint *marked, sqint *positions) {
	// reset to start state. All slices are valid
	char *diskId = config.diskDesc[diskIdx].id;
	const void *bvar[] = {diskId, diskId};
	sqint posRemoved = 0, slicesRemoved = 0, slicesMarked = 0;

	// mark outdated slices positions (contained modified or removed files)
	BEGIN();
	SQL_EXEC("UPDATE slpos AS s SET state = '*' WHERE state = '' AND s.disk = ? AND fpath IN "
		"(SELECT name FROM newfiles AS n WHERE n.disk = ? AND s.fpath = n.name AND "
		"(s.fsize != n.size OR s.mtime != n.mtime OR n.type <> 'f'))",
		NULL, bvar
	);
	SQL_EXEC("UPDATE slpos SET state = '*' WHERE state = '' AND disk = ? AND fpath NOT IN "
		"(SELECT name FROM newfiles WHERE disk = ? AND type = 'f')",
		NULL, bvar);

	// mark outdated slices (with outdated positions)
	SQL_EXEC("CREATE TEMP TABLE outdated AS "
		"SELECT DISTINCT h.sid, h.state FROM slpos p, slhead h WHERE p.sid = h.sid AND p.state = '*'");

	// Remove outdated, not saved
	posRemoved = SQL_EXEC("DELETE FROM slpos WHERE sid IN (SELECT sid FROM outdated o WHERE o.state = '')");
	slicesRemoved = SQL_EXEC("DELETE FROM slhead WHERE state = '' AND sid IN (SELECT sid FROM outdated)");

	// Mark outdated saved. Next sync will remove from archive
	slicesMarked = SQL_EXEC("UPDATE slhead SET state = concat(state, '*') WHERE state NOT LIKE '%*' AND sid IN (SELECT sid FROM outdated)");

	// Mark files that belongs to any VALID slice
	SQL_EXEC("UPDATE newfiles SET state = '*' WHERE disk = ? AND state = '' AND "
		"name in (SELECT fpath FROM slpos WHERE disk = ?)", NULL, bvar);
	SQL_EXEC("DROP TABLE outdated");
	COMMIT();
	*removed += slicesRemoved;
	*marked += slicesMarked;
	*positions += posRemoved;
}

int loadDataFiles() {
	newfileIndex(false);	// drop index
	sqint posRemoved = 0, slicesRemoved = 0, slicesMarked = 0;
	FILE *ownerList = openAt(NOT_OWNER_LIST, "w", config.workDir, true);
	int uid = config.euserId, notOwner = 0, restoreDisk = -1;
	struct passwd *pwd = getpwuid(uid);
	if(config.cmd == RESTORE)
		restoreDisk = getDiskIndex(config.restoreDiskId);
	fprintf(ownerList, "List of files not owned by '%s'\n", pwd->pw_name);
	fprintf(ownerList, "Owner       Size       Type Path\n");
	fprintf(ownerList, "----------- ---------- ---- ------------------------------\n");
	for(int i = 0; i < config.disks; i++) {
		if(i != restoreDisk && diskIsMounted(i)) {
			notOwner += loadFileList(i, ownerList);
			refreshState(i, &slicesRemoved, &slicesMarked, &posRemoved);
		}
		if(config.cmd == SYNC)
			loadLinks(i);
	}
	fclose(ownerList);
	if(notOwner > 0) {
		log_info("WARNING. You are not owner of %d file(s). See '%s' for list. ", notOwner, NOT_OWNER_LIST);
		log_info("WARNING. To fully restore permissions for this files you must run program as ROOT or files owner");
		log_info("WARNING. If future restore file destination disk will support ACL then files permissions will be emulated!");
	} else
		unlinkAt(NOT_OWNER_LIST, config.workDir);
	if(posRemoved || slicesRemoved || slicesMarked)
		log_info("Refresh slices state finished");
	if(posRemoved || slicesRemoved)
		log_info("%ld not saved on disk slices removed (%ld positions)", slicesRemoved, posRemoved);
	if(slicesMarked)
		log_info("%ld saved on disk slices marked outdated", slicesMarked);

	// Create indexes on modified table
	newfileIndex(true);
	sqint maxSize = 0;
	maxSizeDisk = 0;
	for(int i = 0; i < config.disks; i++) {
		if(config.diskDesc[i].filesSize > maxSize) {
			maxSize = config.diskDesc[i].filesSize;
			maxSizeDisk = i;
		}
	}
	if(strcmp(config.workDir, config.configDir)) {
		cleanDir(config.configDir);
		copyConfig(true);
	}
	return 0;
}

int sidChanged(sqint *dsizes) {
	int maxfd = -1, minfd = -1;
	for(int i = 0; i < config.disks; i++) {
//		log_info("Disk D%d size: %ld + %ld", i + 1, cSlice->disks[i].sumFilesLen, dsizes[i]);
		dsizes[i] += cSlice->disks[i].sumFilesLen;
		if(maxfd < 0 || dsizes[maxfd] < dsizes[i])
			maxfd = i;
		if(minfd < 0 || dsizes[minfd] > dsizes[i])
			minfd = i;
	}
	sqint fill = computeFillFactor(dsizes);
//	log_info("Fill factor. Old: %ld   New: %ld", cSlice->fillFactor, fill);
	if(fill > cSlice->fillFactor || (fill == cSlice->fillFactor && dsizes[maxfd] == cSlice->disks[maxfd].sumFilesLen)) {
		// slice accepted
		cSlice->fillFactor = fill;
		for(int i = 0; i < config.disks; i++)
			cSlice->disks[i].sumFilesLen = dsizes[i];
		if(maxfd != cSlice->maxFilledDisk)
			cSlice->maxFilledDisk = maxfd;
		return minfd;
	}
	return -1;
}

static void prepareForPlan() {
	// Remove not perfect slice, not saved on disk
	sqint fill = 100 - config.percentTolerance, deleted;
	const void *bindVars[] = {&fill};
	BEGIN();
	deleted = SQL_EXEC("DELETE FROM slpos WHERE sid IN (SELECT sid FROM slhead h WHERE h.state = '' AND h.fill < :i)", NULL, bindVars);
	if(deleted > 0) {
		log_info("Remove %d position from unsaved skew slices (fill < %d%%)", deleted, fill);
		SQL_EXEC("DELETE FROM slhead WHERE state = '' AND fill < :i", NULL, bindVars);
		SQL_EXEC("UPDATE newfiles SET state = '' WHERE state = '*' AND type = 'f' AND name NOT IN (SELECT fpath FROM slpos)");
	}
	COMMIT();
	// check if SYNC should start
	if(config.cmd == SYNC) {
		// remove unbalanced saved slices where disks are unbalanced
		if(config.dry) {
			log_info("Dry run. Omit optimisation of saved skew slices!");
			return;
		}
		sqint files, fsizes, dsizes[config.disks] = {0};
		int didx;
		SQL_COMMAND grp = {.cmd =
			"SELECT disk, count(*), sum(size) AS sz FROM newfiles WHERE state = '' AND size > 0 AND type = 'f' "
			"GROUP BY disk ORDER by sz DESC"};
		const void *dvar[] = {NULL, &files, &fsizes};
		bzero(cSlice, sizeof(SLICE));
		cSlice->maxFilledDisk = -1;
		while(SQL(&grp, step, dvar) > 0) {
			didx = getDiskIndex((char *) dvar[0]);
			dsizes[didx] = fsizes;
			cSlice->disks[didx].filesInSlice = files;
		}
		SQL(&grp, finalize);
		int minDisk = sidChanged(dsizes);
		if(cSlice->maxFilledDisk < 0 || cSlice->fillFactor >= BALANCED_DISK_FILL_LIMIT)
			return;
		char cmd[512];
		const void *dvars[] = {&files};
		sprintf(cmd, "SELECT count(*) FROM slhead WHERE fill < %d AND state LIKE 'D%c'", BALANCED_DISK_FILL_LIMIT, '%');
		SQL_EXEC(cmd, NULL, NULL, dvars);
		if(files <= 0)
			return;
		sqint maxHole = cSlice->disks[cSlice->maxFilledDisk].sumFilesLen - cSlice->disks[minDisk].sumFilesLen;
		log_info("Try to optimize saved skew slices. Start disk fill factor: %ld%%", cSlice->fillFactor);
//		SQL_EXEC("DROP TABLE IF EXISTS ondisk");
		sprintf(cmd, "CREATE TEMP TABLE ondisk AS SELECT p.sid, p.disk, count(*) as files, sum(fsize) as fsize FROM slpos p, slhead h "
			"WHERE h.fill < %d AND p.sid == h.sid AND h.state LIKE 'D%c' GROUP BY p.sid, p.disk", BALANCED_DISK_FILL_LIMIT, '%');
		SQL_EXEC(cmd);
		const void *bvar[] = {&maxHole};
		SQL_COMMAND scan = {.cmd = "SELECT o.sid, o.disk, o.files, o.fsize, h.ssize FROM ondisk o, slhead h "
			"WHERE h.sid = o.sid AND h.ssize <= :i ORDER BY h.fill, h.ssize desc, h.sid" };
		const void *dvar2[] = {NULL, NULL, &files, &fsizes, &cSlice->sliceSize};
		SQL(&scan, bind, bvar);
		char csid[SLICE_ID_SIZE + 1] = "", *nsid;
		const void *delVars[] = {csid};
		bzero(dsizes, sizeof(dsizes));
		deleted = 0;
		sqint sumSlices = 0;
		StatusBar sb = StatusBar("%d slices encountered. Space to free: %s. ");
		BEGIN();
		while(SQL(&scan, step, dvar2) > 0) {
			nsid = (char *) dvar2[0];
			if(strcmp(csid, nsid)) {
				if(!isBlank(csid)) {
					// modify stats
					minDisk = sidChanged(dsizes);
					if(minDisk >= 0) {
						// Slice accepted to remove
						deleted++;
						sumSlices += cSlice->sliceSize;
						sb.show(1, cSlice->sliceSize, "Disk fill factor: %ld%% ", cSlice->fillFactor);
						SQL_EXEC("DELETE FROM ondisk WHERE sid = ?", NULL, delVars);
						if(!config.dry)
							removeSlice(csid);
						if(cSlice->fillFactor >= BALANCED_DISK_FILL_LIMIT)
							break;
					}
				} else
					minDisk = -1;
				COPY_FLD(csid, nsid);
				bzero(dsizes, sizeof(dsizes));
				if(minDisk >= 0) {
					maxHole = cSlice->disks[cSlice->maxFilledDisk].sumFilesLen - cSlice->disks[minDisk].sumFilesLen;
					SQL(&scan, bind, bvar);
				}
			}
			didx = getDiskIndex((char *) dvar2[1]);
			dsizes[didx] = fsizes;
		}
		SQL(&scan, finalize);
		COMMIT();
		if(deleted) {
			if(!config.dry)
				recalculateNewfiles();
		}
		SQL_EXEC("DROP TABLE ondisk");
	}
}

// First step. Fill slice tables
int createSlices() {
	if(cSlice == NULL)
		cSlice = (SLICE *) malloc(sizeof(SLICE));	// static on Top
	sqint file2process, meanFactor = 0;
	const void *vars[] = {&file2process, &meanFactor};
	SQL_EXEC("SELECT count(*) FROM newfiles WHERE state = '' AND size >= 0 AND type == 'f'", NULL, NULL, vars);
	if(file2process <= 0) {
		log_info("Source data not changed. Generation skipped");
		return 0;	// State not changed
	}
	prepareForPlan();
	SQL_EXEC("SELECT count(*) FROM newfiles WHERE state = '' AND size >= 0 AND type == 'f'", NULL, NULL, vars);
	if(file2process <= 0) {
		log_info("Source data not changed. Generation skipped");
		return 0;
	}
	log_info("Generate slices for %ld backup files ...", file2process);
	bzero(cSlice, sizeof(SLICE));
	strcpy(cSlice->sliceId, SLICE_PREFIX);
	int slcCount = 0;
	TCA *tca = getTCA();
	bzero(tca, sizeof(TCA));
	tca->start = tca->lastTime = time(NULL);
	StatusBar sb = StatusBar("Slices: %d    files: %ld/%ld", 0, 20, file2process);
	while(true) {
		if(cancelRequest(NULL))
			break;
		int solo = -1;
		if(cSlice->sliceSize == 0) {
			if(config.maxSlices && slcCount >= config.maxSlices)
				break;
			BEGIN();
			solo = startSlice(slcCount);
			if(cSlice->sliceSize == 0)
				break;
			slcCount++;
			if(solo >= 0) {
				// Only one disk in slice. Copy all files
				SQL_COMMAND count = {.cmd =
					"SELECT disk, count(*), sum(size) FROM newfiles WHERE state = '' AND size >= 0 AND type == 'f' GROUP BY disk"};
				sqint files, sizes;
				const void *dvars[] = {NULL, &files, &sizes};
				int st = SQL(&count, step, dvars);
				if(st > 0) {
					insert2slice(solo, 0, 0, files, sizes);
					st = SQL(&count, step, dvars);
					if(st > 0)
						log_fatal("Not processed files %ld, sizes: %ld on disk: %s", files, sizes, (char *) dvars[0]);
				}
				SQL(&count, finalize, NULL);
			}
		}
		if(solo >= 0 || addFile2slice() < 0) {
			// slice ready. Clear structures to start next
			cSlice->fillFactor = computeFillFactor();
			sb.show(1, cSlice->filesCount);
			const void *bvars[] = {cSlice->sliceId, &cSlice->sliceSize, &cSlice->filesCount, &cSlice->fillFactor};
			SQL_EXEC("INSERT INTO slhead (sid, ssize, files, fill) VALUES (?, :i1, :i2, :i3)", NULL, bvars);
			COMMIT();
			printSlice(cSlice->sliceId, cSlice->sliceSize, cSlice->filesCount, "", cSlice->fillFactor);
			clearSliceStat(cSlice);
		}
	}
	if(transactionActive())
		ROLLBACK();
	log_info("Slice generation finished. %d slices generated", slcCount);
	if(SQL_EXEC("SELECT sum(ssize), sum((ssize / 100) * fill) FROM slhead WHERE state = ''", NULL, NULL, vars) > 0 && file2process > 0) {
		char wrk[128];
		formatSize(file2process, wrk, false);
		meanFactor = round((double(meanFactor) / file2process) * 100.0);
		log_info("Slices size: %s    Overall fill factor: %ld%%\n", wrk, meanFactor);
	}
	if(tca->operationCanceled)
		slcCount = -1;
	bzero(tca, sizeof(TCA));
	SQL(&insert, finalize);
	SQL(&update, finalize);
	SQL(&add, finalize);
	SQL(&sel, finalize);
	return slcCount;
}
