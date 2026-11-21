#include <sqlite3.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    sqlite3 *db = NULL;
    char *error = NULL;
    int rc;
    if (argc != 2) return 2;
    rc = sqlite3_open(argv[1], &db);
    if (rc == SQLITE_OK)
        rc = sqlite3_exec(db, "PRAGMA journal_mode=WAL; CREATE TABLE probe(value INTEGER);"
                              "PRAGMA wal_checkpoint(TRUNCATE); INSERT INTO probe VALUES(13);",
                          NULL, NULL, &error);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot create WAL fixture: %s\n", error ? error : sqlite3_errmsg(db));
        sqlite3_free(error);
        if (db) sqlite3_close(db);
        return 1;
    }
    _exit(0);
}
