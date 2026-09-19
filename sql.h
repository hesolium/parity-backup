#ifndef SRC_SQL_H_
#define SRC_SQL_H_
#include <stdio.h>
#include <pthread.h>
#include "sqlite3.h"

enum SQL_ACTION {
	prepare = 8, bind, step, reset, lock, unlock, finalize
};

typedef struct {
	const char		*cmd;
	sqlite3 		*db;
	sqlite3_stmt	*stmt;
	int 			recno;
	bool			textMarker;
	void 			*binds[16];
	int				types[16];
	pthread_mutex_t *mutex;
} SQL_COMMAND;

#define SQL_MARKER(array) {array[0] = (char)(sizeof(array)); strncpy(array + 1, "SQL_VAR", sizeof(array) - 1);}

//void logSqlError(const char *file, const int line, const char *fmt, ...);
int sql(SQL_COMMAND *cmd, int action, const char *file, int line, const void *vars[] = NULL);
int sqlImmediate(const char *sqlcmd, const char *file = NULL, int line = 0, sqlite3 *db = NULL, const void *binds[] = NULL, const void *dvars[] = NULL);
void closeDB(sqlite3 *db = NULL);
sqlite3 *openDB(const char *fname, const char *dir = NULL, int readonly = 0);
void setDefaultBase(sqlite3 *db);
sqlite3 *getDefaultBase(void);
int begin(const char *file = NULL, int line = 0, sqlite3 *db = NULL);
int commit(const char *file = NULL, int line = 0, sqlite3 *db = NULL);
int rollback(const char *file = NULL, int line = 0, sqlite3 *db = NULL);
bool transactionActive(sqlite3 *db = NULL);

#define BEGIN() begin(__FILE__, __LINE__ )
#define COMMIT() commit(__FILE__, __LINE__ )
#define ROLLBACK() rollback(__FILE__, __LINE__ )

#define SQL_EXEC(cmd, ...) sqlImmediate(cmd, __FILE__, __LINE__, ##__VA_ARGS__)
#define	SQL(cmd, action, ...) sql(cmd, action, __FILE__, __LINE__, ##__VA_ARGS__)

#endif /* SRC_SQL_H_ */
