#include "backup.h"
#include "sql.h"

#include "sqlite3.h"
static sqlite3 *defaultBase;

bool transactionActive(sqlite3 *db) {
	if(db == NULL)
		db = defaultBase;
	int st = sqlite3_txn_state(db, NULL);
	return (st == SQLITE_TXN_WRITE);
}

int rollback(const char *file, const int line, sqlite3 *db) {
	if(db == NULL)
		db = defaultBase;
	return sqlite3_exec(db, "ROLLBACK", 0, 0, NULL);
}

static bool isUpdate(const char *sql) {
	const char *word = ltrim(sql);
	return strncasecmp(word, "insert", 6) == 0 || strncasecmp(word, "update", 6) == 0 || strncasecmp(word, "delete", 6) == 0;
}

static void dumpBinds(SQL_COMMAND *cmd, const char *file, const int line) {
	if(cmd->stmt) {
		// Dump bind variables
		int bn = sqlite3_bind_parameter_count(cmd->stmt);
		if(bn > 0) {
			log_log(LOG_INFO, file, line, "Binded variables:");
			for(int i = 0; i < bn; i++) {
				if(cmd->binds[i] == NULL)
					continue;
				if(cmd->types[i] == SQLITE_INTEGER) {
					sqint *val = (sqint *) cmd->binds[i];
					log_log(LOG_INFO, file, line, "Var %d: %ld", i, *val);
				} else {
					char *val = (char *) cmd->binds[i];
					int k = strlen(val);
					if(k > 100)
						k = 100;
					log_log(LOG_INFO, file, line, "Var %d: %*s", i, k, val);
				}
			}
		}
	}
}

static void logSqlError(SQL_COMMAND *cmd, const char *file, const int line, const char *fmt, ...) {
	int level = fmt ? LOG_ERROR : LOG_FATAL;
	sqlite3 *db = (cmd->db == NULL) ? defaultBase : cmd->db;
	if(db) {
		int err = sqlite3_errcode(db);
		const char *terr = sqlite3_errmsg(db);
		errno = 0;
		log_log(level, file, line, "SQL error %d : %s", err, terr);
		dumpBinds(cmd, file, line);
	}
	if(fmt) {
		char msg[512] = "SQL Error context: ";
		va_list args;
		va_start(args, fmt);
		vsnprintf(msg + strlen(msg), sizeof(msg) - strlen(msg), fmt, args);
		rollback(file, line, db);
		log_log(LOG_FATAL, file, line, msg);
		va_end(args);
	}
}

void setBindTypes(SQL_COMMAND *cmd) {
	if(cmd->stmt) {
		int bn = sqlite3_bind_parameter_count(cmd->stmt);
		for(int i = 0; i < bn; i++) {
			const char *name = sqlite3_bind_parameter_name(cmd->stmt, i + 1);
			cmd->types[i] = (name && tolower(name[1]) == 'i') ? SQLITE_INTEGER : SQLITE_TEXT;
		}
	}
}

static void sqlBind(SQL_COMMAND *cmd, int recNo, char *values[], int types[], const char *file, int line) {
	int rv = -1;
	sqlite3_stmt *stmt = cmd->stmt;
	int bn = sqlite3_bind_parameter_count(stmt);
	for(int i = 0; i < bn; i++) {
		if(values[i] == NULL)
			logSqlError(cmd, file, line, "NULL bind: %d for statement: %s", i, sqlite3_sql(stmt));
		int type = types[i];
		cmd->binds[i] = values[i];
		if(type == SQLITE_INTEGER) {
			int64_t *val = (int64_t *) values[i];
			rv = sqlite3_bind_int64(stmt, i + 1, *val);
		} else if(type == SQLITE_TEXT) {
			char *val = values[i];
			rv = sqlite3_bind_text(stmt, i + 1, val, -1, SQLITE_STATIC);
		}
		if(rv != SQLITE_OK) {
			const char *sql_text = sqlite3_sql(stmt);
			logSqlError(cmd, file, line, "Bind failed. SQL: %s\n Record: %d, field: %d", sql_text, recNo, i);
		}
	}
	errno = 0;
}

static void checkSqlError(sqlite3 *db, const char *file, const int line, const char *fmt, ...) {
	int err = sqlite3_errcode(db);
	if(err != SQLITE_OK && err != SQLITE_ROW && err != SQLITE_DONE) {
		errno = 0;
		const char *terr = sqlite3_errmsg(db);
		int level = fmt ? LOG_ERROR : LOG_FATAL;
		log_log(level, file, line, "SQL error %d : %s", err, terr);
		if(fmt) {
			char msg[512] = "SQL Error context: ";
			va_list args;
			va_start(args, fmt);
			vsnprintf(msg + strlen(msg), sizeof(msg) - strlen(msg), fmt, args);
			rollback(file, line, db);
			log_log(LOG_FATAL, file, line, msg);
			va_end(args);
		}
	}
}

int sql(SQL_COMMAND *cmd, int action, const char *file, int line, const void *vars[]) {
	int n, rv = 0;
	char **cvars = (char **) vars;
	sqlite3 *db = cmd->db;

	if(db == NULL)
		db = defaultBase;
	switch(action) {
	case lock:
		if(cmd->mutex != NULL && pthread_mutex_lock(cmd->mutex))
			log_fatal("SQL mutex lock failed");
		break;

	case unlock:
		if(cmd->mutex != NULL && pthread_mutex_unlock(cmd->mutex))
			log_fatal("SQL mutex unlock failed");
		break;

	case finalize:
	case prepare:
		if(cmd->stmt) {
			rv = sqlite3_finalize(cmd->stmt);
			if(rv)
				checkSqlError(db, file, line, "%s", cmd->cmd);
			cmd->stmt = NULL;
		}
		if(action == prepare) {
			rv = sqlite3_prepare_v2(db, cmd->cmd, -1, &cmd->stmt, NULL);
			checkSqlError(db, file, line, "%s", cmd->cmd);
			setBindTypes(cmd);
		}
		break;

	case reset:
	case bind:
		if(cmd->stmt == NULL) {
			sqlite3_prepare_v2(db, cmd->cmd, -1, &cmd->stmt, NULL);
			checkSqlError(db, file, line, "%s", cmd->cmd);
			setBindTypes(cmd);
		} else
			sqlite3_reset(cmd->stmt);
		if(action == bind) {
			n = sqlite3_bind_parameter_count(cmd->stmt);
			if(n > 0) {
				if(vars == NULL)
					log_fatal("No variables bound to SQL statement %s", cmd->cmd, file, line);
				sqlBind(cmd, cmd->recno, cvars, cmd->types, file, line);
			}
		}
		checkSqlError(db, file, line, "%s", cmd->cmd);
		break;

	case step:
		if(cmd->stmt == NULL) {
			sqlite3_prepare_v2(db, cmd->cmd, -1, &cmd->stmt, NULL);
			checkSqlError(db, file, line, "%s", cmd->cmd);
		}
		rv = sqlite3_step(cmd->stmt);
		if(rv == SQLITE_ROW) {
			if(vars == NULL)
				log_fatal(file, line, "No destination array for result from command: %s", cmd->cmd);
			int columns = sqlite3_column_count(cmd->stmt);
			for(int i = 0; i < columns; i++) {
				int tp = sqlite3_column_type(cmd->stmt, i);
				if(tp == SQLITE_INTEGER) {
					if(vars[i] == NULL)
						logSqlError(cmd, file, line, "Column %d address not set!. SQL:%s", i, sqlite3_sql(cmd->stmt));
					sqint val = sqlite3_column_int64(cmd->stmt, i);
					sqint *addr = (sqint *) vars[i];
					*addr = val;
				}
				else if(tp == SQLITE_TEXT) {
					char *val = (char *) sqlite3_column_text(cmd->stmt, i);
					if(vars[i] == NULL || ! cmd->textMarker)
						vars[i] = val;
					else {	// destinetion area in format 1-byte buffer size and SQL_VAR text
						char *target = (char *) vars[i];
						int vsize = *target;
						bool err = true;
						int slen = strlen(val);
						if(vsize > 0 && slen  < vsize)
							err = (strncmp(target + 1, "SQL_VAR", vsize) != 0);
						if(err)
							logSqlError(NULL, file, line,
									"Destination %d TEXT address not initialized!. SQL:%s", i, sqlite3_sql(cmd->stmt));
						strcpy(target, val);
					}
				}
				else if(tp == SQLITE_FLOAT) {
					if(vars[i] == NULL)
						logSqlError(NULL, file, line, "Column %d address not set!. SQL:%s", i, sqlite3_sql(cmd->stmt));
					double val = sqlite3_column_double(cmd->stmt, i);
					double *addr = (double *) vars[i];
					*addr = val;
				}
				else if(tp == SQLITE_NULL)
					vars[i] = 0;
			}
			cmd->recno++;
			rv = 1;
		} else if(rv == SQLITE_DONE) {
			rv = (isUpdate(cmd->cmd)) ? sqlite3_changes(db) : 0;
		} else
			logSqlError(cmd, file, line, "step failed");
		break;
	}
	errno = 0;
	return rv;
}

int sqlImmediate(const char *sqlcmd, const char *file, int line, sqlite3 *db, const void *binds[], const void *dvars[]) {
	int rv = 0;
	if(db == NULL)
		db = defaultBase;
	if(binds == NULL && dvars == NULL) {
		sqlite3_exec(db, sqlcmd, 0, 0, NULL);
		if(isUpdate(sqlcmd))
			rv = sqlite3_changes(db);
		checkSqlError(db, file, line, "%s", sqlcmd);
	} else {
		SQL_COMMAND scmd = {.cmd = sqlcmd, .db = db, .textMarker = true};
		sql(&scmd, bind, file, line, binds);
		rv = sql(&scmd, step, file, line, dvars);
		sql(&scmd, finalize, file, line);	// text variables not valid
	}
	return rv;
}

int begin(const char *file, int line, sqlite3 *db) {
	return sqlImmediate("BEGIN IMMEDIATE", file, line, db);
}

int commit(const char *file, int line, sqlite3 *db) {
	return sqlImmediate("COMMIT", file, line, db);
}

sqlite3 *openDB(const char *fname, const char *dir, int readonly) {
	char dbPath[PATH_MAX] = "";
	if(dir)
		strcpy(dbPath, dir);
	strcat(dbPath, fname);
	readonly = (readonly == 0) ? SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE : SQLITE_OPEN_READONLY;
	sqlite3 *db = NULL;
	int ret = sqlite3_open_v2(dbPath, &db, readonly, NULL);
	if(ret != SQLITE_OK)
		log_fatal("Open database %s error %d", dbPath, ret);
	return db;
}

void closeDB(sqlite3 *pdb) {
	sqlite3 *db = pdb;
	if(db == NULL)
		db = defaultBase;
	if(db) {
		if(transactionActive(db)) {
			log_warn("Transaction in progress\n");
			ROLLBACK();
		}
		int n = 0;
		sqlite3_stmt *stmt = NULL, *toFinalize[16] = {NULL};
		while(true) {
			stmt = sqlite3_next_stmt(db, stmt);
			if(stmt == NULL)
				break;
			log_warn("Unfinalized statement: %s\n", sqlite3_sql(stmt));
			toFinalize[n++] = stmt;
		}
		for(int i = 0; i < n; i++)
			sqlite3_finalize(toFinalize[i]);
		const char *dbn = sqlite3_db_filename(db, NULL);
		if(dbn == NULL)
			dbn = "";
		verbose("Close database %s\n", dbn);
		int st = sqlite3_close(db);
		if(st != SQLITE_OK) {
			errno = 0;
			log_warn("Database %s not closed gracefully. Error %d: %s", st, sqlite3_errmsg(db));
		}
		if(db == defaultBase)
			defaultBase = NULL;
	}
}

void setDefaultBase(sqlite3 *db) {
	defaultBase = db;
}

sqlite3 *getDefaultBase(void) {
	return defaultBase;
}
