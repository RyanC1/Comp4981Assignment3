#ifndef DB_UTIL_H
#define DB_UTIL_H

/*
 * db_util.h — thin, safe wrapper around ndbm.
 *
 * Both the CLI tool (main.c) and the HTTP server (http.c) use this
 * interface so that all raw DBM / datum handling lives in one place.
 *
 * Ownership rules
 * ---------------
 * - db_fetch / db_first / db_next return pointers into ndbm's *internal*
 *   static buffer.  The caller must NOT free them and must copy the data
 *   before calling any other db_* function.
 * - db_store, db_delete, db_open, db_close are safe to call from any
 *   thread provided the caller serialises access externally (e.g. with a
 *   semaphore).
 */

#include <stddef.h>

#define DATABASE_NAME "data.db" // NOLINT(cppcoreguidelines-macro-to-enum, modernize-macro-to-enum)
#define DATABASE_NAME_LEN 8 // NOLINT(cppcoreguidelines-macro-to-enum, modernize-macro-to-enum)
#define DATABASE_MODE 0666 // NOLINT(cppcoreguidelines-macro-to-enum, modernize-macro-to-enum)

/* Opaque handle – zero-initialise before first use. */
typedef struct
{
    void *dbm; /* DBM* hidden so callers need not include <ndbm.h> */
} DbHandle;

/* --------------------------------------------------------------------- */
/* Lifecycle                                                               */
/* --------------------------------------------------------------------- */

/*
 * db_open: open (or create) the database at `path`.
 * Returns  0 on success, -1 on failure (errno set by dbm_open).
 */
int db_open(DbHandle *out);

/*
 * db_close: close the database and NULL out the internal pointer.
 * Safe to call on a handle whose dbm pointer is already NULL.
 */
void db_close(DbHandle *h);

/* --------------------------------------------------------------------- */
/* CRUD                                                                    */
/* --------------------------------------------------------------------- */

/*
 * db_store: insert or replace (key, val).
 * Returns  0 on success, -1 on error.
 * Errors: h/key/val NULL, key_len == 0, or underlying dbm_store failure.
 */
int db_store(DbHandle *h,
             const void *key, size_t key_len,
             const void *val, size_t val_len);

/*
 * db_fetch: look up a key.
 * On success sets *val_out and *val_len_out (either may be NULL if the
 * caller doesn't need them) and returns 1 (found) or 0 (not found).
 * Returns -1 on hard error (bad arguments or dbm error).
 * The returned pointer is ndbm-internal; copy before the next db_* call.
 */
int db_fetch(DbHandle *h,
             const void *key, size_t key_len,
             void **val_out, size_t *val_len_out);

/*
 * db_delete: remove a key.
 * Returns 0 on success, -1 on failure (including key-not-found on some
 * ndbm implementations).
 */
int db_delete(DbHandle *h, const void *key, size_t key_len);

/* --------------------------------------------------------------------- */
/* Iteration                                                               */
/* --------------------------------------------------------------------- */

/*
 * db_first / db_next: iterate over all keys in undefined order.
 *
 * Returns 1 and populates *key_out / *key_len_out when a key is available.
 * Returns 0 when iteration is exhausted.
 * Returns -1 on error.
 *
 * The key pointer is ndbm-internal; copy before calling db_fetch or
 * db_next, as either may invalidate it.
 *
 * Typical loop:
 *   void  *k;
 *   size_t klen;
 *   for (int r = db_first(&h, &k, &klen); r == 1; r = db_next(&h, &k, &klen))
 *   { ... }
 */
int db_first(DbHandle *h, void **key_out, size_t *key_len_out);
int db_next(DbHandle *h,  void **key_out, size_t *key_len_out);

#endif /* DB_UTIL_H */
