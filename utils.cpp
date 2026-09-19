#include <sys/stat.h>
#include <dirent.h>
#include <printf.h>
#include <sys/acl.h>
#include "logger.h"
#include "utils.h"
#include "sql.h"

#define XXH_INLINE_ALL
#include "xxhash.h"

//#include "rapidhash.h"

//static XXH64_state_t* hashState = NULL;

uint64_t getHashStateLen(void *hstate) {
	struct XXH64_state_s *state = (XXH_NAMESPACEXXH64_state_t *) hstate;
	return state->total_len;
}

void *allocHashState() {
	XXH64_state_t* hashState = XXH64_createState();
	return hashState;
}

void freeHashState(void *hstate) {
	XXH64_freeState((XXH_NAMESPACEXXH64_state_t *) hstate);
}

int startBlockHash(void *hashState) {
	if (XXH64_reset((XXH_NAMESPACEXXH64_state_t*) hashState, 0) == XXH_ERROR)
		return -1;
	return 0;
}

int updateBlockHash(void *hashState, const char *in_buf, int len) {
	if (len > 0 && XXH64_update((XXH_NAMESPACEXXH64_state_t*) hashState, in_buf, len) == XXH_ERROR)
		return -1;
	return 0;
}

XXH64_hash_t getBlockHash(void *hashState, char *outBuf) {
	XXH64_hash_t rv = XXH64_digest((XXH_NAMESPACEXXH64_state_t*) hashState);
	if(outBuf)
		sprintf(outBuf, "%0*lx", HASH_SIZE_DIGITS, rv);
	return rv;
}

XXH64_hash_t getHash(const char *in_buf, int len, char *outBuf) {
	if(len <= 0)
		len = strlen(in_buf);
	XXH64_hash_t rv = XXH64(in_buf, len, 0);
	if(outBuf)
		sprintf(outBuf, "%0*lx", HASH_SIZE_DIGITS, rv);
	return rv;
}

char *getArgsLine(int argc, const char * const argv[]) {
	char *buf = (char *) malloc(128 * KILO);
	const char *c;
	if(buf) {
		int n = 0, i, k;
		for(i = 1; i< argc; i++) {
			c = (strchr(argv[i], ' ')) ? "'" : "";
			k = sprintf(buf + n, "%s%s%s ", c, argv[i], c);
			n += k;
		}
	}
	return buf;
}

int findUser(const char *userId, const char **uname) {
	struct passwd *pwd = getpwnam(userId);
	int uid = -1;
	if(pwd == NULL) {
		uid = atoi(userId);
		pwd = getpwuid(uid);
	}
	if(pwd == NULL) {
		if(uname)
			*uname = userId;
		return -1;
	}
	uid = pwd->pw_uid;
	if(uname)
		*uname = pwd->pw_name;
	return uid;
}

int excludeSysPath(char *sysPaths, int bufLen, const char *dir) {
	bzero(sysPaths, bufLen);
	char ps = getPathSeparator();
	strcpy(sysPaths, dir);
	int k = strlen(sysPaths);
	if(sysPaths[k - 1] == ps)
		sysPaths[k - 1] = 0;
	k = (sysPaths[0] == ps) ? 1 : 0;
	return k;
}

void expandPhrases(char *buf, int bufSize, const char *phrase, const char *tokens) {
	if(tokens) {
		if(phrase == NULL)
			PERROR(LOG_FATAL, "No phrase defined for include/exclude '%s'", tokens);
		int olen = strlen(buf);
		const char *p = tokens;
		while(*p) {
			olen += snprintf(buf + olen, bufSize - olen, "%s ", phrase);
			olen = replaceStr(buf, bufSize, "$PATH", p);
			if(olen < 0)
				log_fatal("Too big external command: %.100s", buf);
			p += strlen(p) + 1;
		}
	}
}

int monthNumber(const char *mname) {
	static const char *ml = "JFMASOND";
	const char *p = strchr(ml, toupper(mname[0]));
	return (p) ? (p - ml) + 1 : 0;
}

char *splitPath(char *path) {
	char *rv = strrchr(path, getPathSeparator());
	if(rv) {
		*rv = 0;
		return rv + 1;
	}
	return path;
}

const char *fileBasename(const char *path) {
	const char *rv = strrchr(path, getPathSeparator());
	if(rv)
		return rv + 1;
	return path;
}

void replaceChars(char *str, const char *findChars, const char *replaceChars) {
	char c, *p, wrk[strlen(str)], *q = wrk;
	const char *r;
	int idx, repLen = (replaceChars) ? strlen(replaceChars) : 0;
	for(p = str; *p; p++) {
		r = strchr(findChars, *p);
		c = *p;
		if(r) {
			c = 0;
			if(repLen > 0) {
				idx = r - findChars;
				if(idx < repLen)
					c = replaceChars[idx];
			}
			if(c)
				*q++ = c;
		}
		else
			*q++ = c;
	}
	idx = q - wrk;
	memcpy(str, wrk, idx);
	str[idx] = 0;
}

// For 'small' buffers. Local buffer on stack.
int replaceStr(char *buf, int bufSize, const char *from, const char *to) {
	char *p, *start = buf, locBuf[bufSize], *dest = locBuf;
	int csize = 0, len, fromLen = strlen(from), toLen = strlen(to);
	while((p = strstr(start, from))) {
		len = p - start;
		csize += len + toLen;
		if(csize >= bufSize)
			return -1;
		memcpy(dest, start, len);
		dest += len;
		memcpy(dest, to, toLen);
		dest += toLen;
		start += len + fromLen;
	}
	csize += strlen(start);
	if(csize >= bufSize)
		return -1;
	strcpy(dest, start);
	if(csize > 0) {
		memcpy(buf, locBuf, csize);
		buf[csize] = 0;
	}
	else
		csize = strlen(buf);
	return csize;
}

int findAnyChar(const char *str, const char *chars) {
	while(*chars) {
		if(strchr(str, *chars))
			break;
		chars++;
	}
	return *chars;
}

void lower(char *str) {
	while(*str) {
		*str = tolower(*str);
		str++;
	}
}

char *ltrim(const char *s) {
	while(isspace(*s))
		s++;
	return (char *) s;
}

void rtrim(char *s) {
	char *p = s + strlen(s) - 1;
	while(p >= s && isspace(*p))
		*p-- = 0;
}

void rtrimChar(char *s, int c) {
	char *p = s + strlen(s) - 1;
	while(p >= s && *p == c)
		*p-- = 0;
}

char *alltrim(char *s) {
	rtrim(s);
	return ltrim(s);
}

int isPrefix(const char *pref, const char *str) {
	int i;
	for(i = 0; pref[i] && str[i] && pref[i] == str[i]; i++);
	return (pref[i] == 0);
}

void removeFromQuotes(char *str, const char *charsOnly) {
    int inQuotes = 0; // Flag to track if we are inside quotes
    char result[1024]; // Buffer to store the result
    int j = 0; // Index for result

    for (int i = 0; str[i] != '\0'; i++) {
        if (str[i] == '\"' || str[i] == '\'') {
        	if(inQuotes == str[i]) {
        		inQuotes = 0;
        		continue;
        	}
        	else if(inQuotes == 0)
        		inQuotes = str[i]; // Toggle the inQuotes flag ON
        }
        if(inQuotes) {
        	if(charsOnly == NULL)
        		continue;
        	if(strchr(charsOnly, str[i]))
        		continue;
        }
		result[j++] = str[i]; // Add character to result if not in quotes
    }
    result[j] = '\0'; // Null-terminate the result string
    strcpy(str, result); // Copy the result back to the original string
}

int split(char *line, char *tokens[], int tsize, const char *delim, int tcount) {
	char *tok = strtok(line, delim);
	int n = 0;
	if(tcount > tsize)
		tcount = 0;
	while(tok && n <= tsize) {
		tokens[n++] = tok;
		if(tcount && n >= tcount - 1)
			delim = "";
		tok = strtok(NULL, delim);
	}
	return n;
}

int formatSize(sqint size, char *outBuf, bool roundResult) {
	double dsize;
	int idx = 0, step = 1024, rv;
	char suffix[] = "BKMGT";
	sqint mn = step;
	while (size > mn) {
		mn *= step;
		idx++;
	}
	mn /= 1024;
	dsize = (double) size / (double) mn;
	if(roundResult) {
		step = int(round(dsize));
		rv = sprintf(outBuf, "%d ", step);
	} else
		rv = sprintf(outBuf, "%.2f ", dsize);
	if(idx > 0)
		outBuf[rv++] = suffix[idx];
	outBuf[rv++] = 'B';
	outBuf[rv++] = ' ';
	outBuf[rv] = 0;
	return rv;
}

char *formatFileSize(sqint size) {
	static char result[64];
	int k = 0;
	sqint bl;
	const char *plus = "";
	bl = size / GIGA;
	if(bl) {
		k = sprintf(result, "%s%ldg", plus, bl);
		size = size - bl * GIGA;
		plus = "+";
	}
	bl = size / MEGA;
	if(bl) {
		k += sprintf(result + k, "%s%ldm", plus, bl);
		size -= bl * MEGA;
		plus = "+";
	}
	bl = size / KILO;
	if(bl) {
		k += sprintf(result + k, "%s%ldk", plus, bl);
		size -= bl * KILO;
		plus = "+";
	}
	if(size || plus[0] == 0)
		k += sprintf(result + k, "%s%ld", plus, size);
	return result;
}

int readFD(int fd, char *buf, int bufSize) {
	int rest = bufSize, rv = 0;
	char *lbuf = buf;
	while(rest > 0) {
		rv = read(fd, lbuf, rest);
		if(rv == 0)
			break;	// EOF
		if(rv < 0) {
			if(errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
				return -errno;
		} else {	// rv > 0
			rest -= rv;
			lbuf += rv;
		}
	}
	return bufSize - rest;
}

int writeFD(int fd, char *buf, int bufSize) {
	int rest = bufSize, rv = 0;
	char *lbuf = buf;
	while(rest > 0) {
		rv = write(fd, lbuf, rest);
		if(rv == 0)
			break;
		if(rv < 0) {
			if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
				continue;
			return -errno;
		}
		rest -= rv;
		lbuf += rv;
	}
	return bufSize - rest;
}

int sec2hours(int sec, char *buf) {
	int minutes = sec / 60;
	sec = sec % 60;
	int hours = minutes / 60;
	minutes = minutes % 60;
	return sprintf(buf, "%2.2d:%2.2d:%2.2d", hours, minutes, sec);
}

sqint checkTimestamp(const char *ts) {
	struct tm info;
	sqint ns;
	int k = sscanf(ts, "%4d-%2d-%2d_%2d:%2d:%2d.%ld",
		&info.tm_year, &info.tm_mon, &info.tm_mday, &info.tm_hour, &info.tm_min, &info.tm_sec, &ns);
	if(k != 7)
		return -1;
	info.tm_year -= 1900;
	sqint sec = mktime(&info);
	if(sec < 0)
		return sec;
	return sec * 1000000000 + ns;
}

sqint getTimestamp(char *buf, struct timespec *in, int bufSize) {
	struct timespec ts = {0};
	buf[0] = 0;
	if(in == NULL) {
		in = &ts;
		if(clock_gettime(CLOCK_REALTIME, &ts))
			return -1;
	}
	struct tm info;
	if(gmtime_r(&in->tv_sec, &info) == NULL)
		return -1;
	int len = strftime(buf, bufSize, "%Y-%m-%d_%H:%M:%S", &info);
	buf[len] = '\0';
	snprintf(buf + len, bufSize - len, ".%9.9ld", in->tv_nsec);
	return in->tv_sec * 1000000000 + in->tv_nsec;
}

sqint str2timestamp(const char *str, timespec *ts) {
	char *p = NULL;
	sqint rv = strtoll(str, &p, 0);
	if(ts) {
		ts->tv_sec = rv;
		if(p)
			ts->tv_nsec = strtol(p, NULL, 0);
	}
	return rv;
}

sqint timestamp2str(const char *seconds, char *buf, int bufSize) {
	struct timespec time;
	buf[0] = 0;
	if(sscanf(seconds, "%ld", &time.tv_sec) != 1)
		return -1;
	const char *p = strchr(seconds, '.');
	if(p) {
		p++;
		time.tv_nsec = atol(p);
	}
	return getTimestamp(buf, &time, bufSize);
}

sqint getTimestamp(char *buf, struct timespec *in) {
	struct timespec ts = {0};
	if(in == NULL) {
		in = &ts;
		if(clock_gettime(CLOCK_REALTIME, &ts))
			return -1;
	}
	if(buf)
		snprintf(buf, TIMESTAMP_SIZE + 1, "%10.10ld.%9.9ld", in->tv_sec, in->tv_nsec);
	return in->tv_sec * 1000000000 + in->tv_nsec;
}

//switch (sb.st_mode & S_IFMT) {
//           case S_IFBLK:  printf("block device\n");            break;
//           case S_IFCHR:  printf("character device\n");        break;
//           case S_IFDIR:  printf("directory\n");               break;
//           case S_IFIFO:  printf("FIFO/pipe\n");               break;
//           case S_IFLNK:  printf("symlink\n");                 break;
//           case S_IFREG:  printf("regular file\n");            break;
//           case S_IFSOCK: printf("socket\n");                  break;
//           default:       printf("unknown?\n");                break;
//           }
int fileType(int fd) {
	struct stat statbuf = {0};
	sqint rv = fstat(fd, &statbuf);
	if(rv < 0)
		return rv;
	return statbuf.st_mode & S_IFMT;
}

sqint fileInfo(int fd, char *timestamp) {
	struct stat statbuf = {0};
	sqint rv = fstat(fd, &statbuf);
	if(rv < 0)
		return rv;
	if(timestamp)
		getTimestamp(timestamp, &statbuf.st_mtim);
	return statbuf.st_size;
}

sqint fileInfo(FILE *f, char *timestamp) {
	return fileInfo(fileno(f));
}

sqint fileInfo(const char *dir, const char *fname, char *timestamp) {
	char path[PATH_MAX];
	struct stat statbuf = {0};
	normPath(fname, path, dir);
	sqint rv = stat(path, &statbuf);
	if(rv < 0) {
		if(timestamp)
			timestamp[0] = 0;
		return rv;
	}
	if(timestamp)
		getTimestamp(timestamp, &statbuf.st_mtim);
	return statbuf.st_size;
}

// Substitute single apostrophe
int path4shell(char *path, int size) {
	int rv = strlen(path);
	if(strchr(path, '\'')) {
		rv = replaceStr(path, size, "'", "'\"'\"'");
	}
	if(rv >= 0 && rv < size - 3) {
		memmove(path + 1, path, rv + 1);
		rv++;
		path[0] = path[rv++] = '\'';
		path[rv] = 0;
	}
	return rv;
}

int getDiskIndex(const char *disk) {
	if(disk[0] != 'D' || !isdigit(disk[1]))
		return -1;
	int rv = disk[1] - '0';
	const char *p = disk + 2;
	if(isdigit(*p)) {
		rv += rv * 10 + *p - '0';
		p++;
	}
	if(*p && *p != getPathSeparator() && *p != '=')
		return -1;
	return rv - 1;
}

bool isACLactive(const char *path) {
	acl_t acl = acl_get_file(path, ACL_TYPE_ACCESS);
	if(acl) {
		acl_free(acl);
		return true;
	}
	return false;
}

void normPath(const char *fname, char *path, const char *dir) {
	struct passwd *pwd = NULL;
	path[0] = 0;
	if(fname[0] == '/') {
		strcpy(path, fname);
		return;
	}
	if(fname[0] == '~') {
		pwd = getpwuid(geteuid());
		sprintf(path, "%s%s", pwd->pw_dir, fname + 1);
		return;
	}
	if(dir) {
		if(dir[0] == '~') {
			if(pwd == NULL)
				pwd = getpwuid(geteuid());
			sprintf(path, "%s%s", pwd->pw_dir, dir + 1);
			strcat(path, fname);
			return;
		}
		strcpy(path, dir);
	}
	strcat(path, fname);
}

bool isUUID(const char *uuid) {
	for (char c = *uuid; c; c = *++uuid) {
		if(c != '-' && !isxdigit(c))
			return false;
	}
	return true;
}

const char *getMountDir(const char *path) {
	char cmd[PATH_MAX];
	if(isUUID(path))
		sprintf(cmd, "findmnt -rn -S UUID=%s -o TARGET", path);
	else
		sprintf(cmd, "findmnt -rn -T %s -o TARGET", path);
	FILE *f = getOutput(cmd);
	if(f == NULL)
		return NULL;
	int n = getOutputLine(f, cmd, sizeof(cmd));
	if(n < 0)
		return NULL;
	addPathSeparator(cmd);
	pclose(f);
	return strdup(cmd);
}

int procCores() {
    char line[256], *p;
    int cpu_cores = 1;
    FILE *fp = fopen("/proc/cpuinfo", "r");
    if (fp == NULL) {
        log_error("Failed to open /proc/cpuinfo");
        return cpu_cores;
    }
    while (fgets(line, sizeof(line), fp)) {
    	p = strstr(line, "cpu cores");
        if (p) {
        	p = strchr(p, ':');
        	if(p)
        		cpu_cores = strtol(p + 1, NULL, 0);
            break;
        }
    }
    fclose(fp);
    return cpu_cores;
}

int unlinkAt(const char *fname, const char *dir) {
	if(dir) {
		char path[PATH_MAX];
		snprintf(path, sizeof(path), "%s%s", dir, fname);
		return unlink(path);
	}
	return unlink(fname);
}

int openAt(const char *fname, int mode, const char *dir, bool fatal) {
	char path[PATH_MAX];
	normPath(fname, path, dir);
	int rv = 0;
	if(mode & O_CREAT)
		rv = open(path, mode, S_IRUSR | S_IWUSR);
	else
		rv = open(path, mode);
	if(rv < 0 && fatal)
		PERROR(LOG_FATAL, "Can't open file %s", path);
	return rv;
}

FILE *openAt(const char *fname, const char *mode, const char *dir, bool fatal) {
	char path[PATH_MAX];
	normPath(fname, path, dir);
	FILE *rv = fopen(path, mode);
	if(rv == NULL && fatal)
		PERROR(LOG_FATAL, "Can't open file %s", path);
	return rv;
}

int getPathSeparator(char *buf) {
	if(buf) {
		buf[0] = '/';
		buf[1] = 0;
	}
	return '/';
}

sqint stdDeviation(sqint values[], int vsize) {
	double sum = 0, sumsqr = 0, val;
	for(int i = 0; i < vsize; i++){
		val = double(values[i]);
		sum += val;
		sumsqr += val * val;
	}
	double mean = sum / vsize;
	double meanofsquares = sumsqr / vsize;
	double squareofmean = mean * mean;
	double variance = meanofsquares - squareofmean;
	double sd = sqrt(variance);
	return sqint(round(sd));
}

int addPathSeparator(char *path) {
	int n = strlen(path), ps = getPathSeparator();
	if(path[n - 1] != ps) {
		path[n++] = ps;
		path[n] = 0;
	}
	return n;
}

sqint linesInFile(FILE *file, bool fromBegin) {
    char buf[32 * KILO];
    int counter = 0;
    off_t pos = ftell(file);
    if(fromBegin)
    	fseek(file, 0, SEEK_SET);
    while(fgets(buf, sizeof(buf), file) != NULL)
		counter++;
    if(ferror(file))
		log_fatal("Counting file lines");
    fseek(file, pos, SEEK_SET);
    return counter;
}

int getOutputLine(FILE *fp, char *buf, int bufLen) {
	char *p = fgets(buf, bufLen, fp);
	if(p == NULL) {
		int rv = -1;
		if(ferror(fp))
			rv = log_fatal("Reading 'popen'");
		pclose(fp);
		return rv;
	}
	int len = strlen(buf);
	if(len && buf[len - 1] == '\n') {
		len--;
		buf[len] = 0;
	}
	return len;
}

int removeLastNewline(char *line) {
	int pl = strlen(line);
	if(line[pl - 1] == NEWLINE) {
		line[pl - 1] = 0;
		pl--;
	}
	return pl;
}

bool isBlank(const char *str) {
	if(str == NULL)
		return true;
	while(*str && isspace(*str++));
	return (*str == 0) ? true : false;
}

void hexDump(FILE *fp, void *addr, int len, const char *desc)
{
	if(len <= 0 || fp == NULL)
		return;
    int i;
    unsigned char buff[17];
    unsigned char *pc = (unsigned char*)addr;

    // Output description if given.
    if(!isBlank(desc))
        fprintf (fp, "%s\n", desc);
    if(desc == NULL)
    	desc = "";

    // Process every byte in the data.
    for (i = 0; i < len; i++) {
        // Multiple of 16 means new line (with line offset).

        if ((i % 16) == 0) {
            // Just don't print ASCII for the zeroth line.
            if (i != 0)
                fprintf(fp, "  %s\n", buff);

            // Output the offset.
            fprintf(fp, "%s  %04x ", desc, i);
        }
        // Now the hex code for the specific character.
        fprintf(fp, " %02x", pc[i]);
        // And store a printable ASCII character for later.
        if ((pc[i] < 0x20) || (pc[i] > 0x7e))
            buff[i % 16] = '.';
        else
            buff[i % 16] = pc[i];
        buff[(i % 16) + 1] = '\0';
    }
    // Pad out last line if not exactly 16 characters.
    while ((i % 16) != 0) {
        fprintf(fp, "   ");
        i++;
    }
    // And print the final ASCII bit.
    fprintf(fp, "  %s\n", buff);
}
// -------------------------------------------------------

StatusBar::StatusBar(const char *fmt, int maxElem, int showStep, sqint maxSize) {
	maxElements = maxElem;
	maxVolume = maxSize;
	if(showStep && maxElem <= 0 && maxSize <= 0)
		log_fatal("Inconsistent create StatusBar!");
	step = MAX(showStep, 1);
	elements = volume = 0;
	strncpy(format, fmt, sizeof(format));
	fcount = parse_printf_format(fmt, 4, types);
	lastTime = time(NULL);
}

void StatusBar::show(int plusElements, sqint plusVolume, const char *fmt, ...) {
	extern bool showStatusBar;
	if(!showStatusBar)
		return;
	char msg[PATH_MAX];
	elements += plusElements;
	volume += plusVolume;
	if(step) {
		if(!((maxElements && elements == maxElements) || (maxVolume && volume == maxVolume)))
			if((elements % step != 0))
				return;
	}
	char volStr[20] = "", maxVolStr[20] = "";
	if(volume > 0 || maxVolume > 0) {
		formatSize(volume, volStr, false);
		formatSize(maxVolume, maxVolStr, false);
	}
	sqint val[MAX_FIELDS] = {0};
	int cInt = 0, cLong = 0, cStr = 0, btp, flag;
	for(int i = 0; i < fcount; i++) {
		btp = types[i] & ~PA_FLAG_MASK;
		flag = types[i] & PA_FLAG_MASK;
		switch (btp) {
			case PA_INT:
				if(flag == PA_FLAG_LONG) {
					if(cLong == 0)
						val[i] = volume;
					else
						val[i] = maxVolume;
					cLong++;
				} else {
					if(cInt == 0)
						val[i] = (sqint) elements;
					else
						val[i] = (sqint) maxElements;
					cInt++;
				}
				break;
			case PA_STRING:
				if(cStr == 0)
					val[i] = (sqint) volStr;
				else
					val[i] = (sqint) maxVolStr;
				cStr++;
				break;
			default:
				log_fatal("Not allowed field type: %d", types[i]);
		}
	}
	int k = sprintf(msg, format, val[0], val[1], val[2], val[3], val[4], val[5]);
	if(fmt) {
		va_list args;
		va_start(args, fmt);
		vsnprintf(msg + k, sizeof(msg) - k, fmt, args);
		va_end(args);
	}
	log_info("\r%s    ", msg);
}

// -------------------------------------------------------
TinyQueue::TinyQueue(int qsize) {
	rowids = (sqint *) calloc(qsize, sizeof(sqint));
	size = qsize;
}


TinyQueue::~TinyQueue() {
	if(rowids)
		free(rowids);
}

void TinyQueue::clear() {
	head = tail = 0;
	bzero(rowids, sizeof(sqint) * size);
}

void *TinyQueue::pop() {
	void *rv = NULL;
	if(head != tail)
		return (void *) rowids[tail++];
	return rv;
}

int TinyQueue::push(void *job) {
	if(head < size) {
		rowids[head++] = (sqint) job;
		return head;
	}
	return -1;
}

bool TinyQueue::isFull() {
	return head == size;
}

int TinyQueue::getCount() {
	return head - tail;
}
