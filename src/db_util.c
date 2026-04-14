#include "db_util.h"
#include <fcntl.h>
#include <ndbm.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * All functions that return a datum (dbm_fetch, dbm_firstkey, dbm_nextkey)
 * trigger -Waggregate-return.  Isolate them here so the suppression is
 * scoped as tightly as possible and no other translation unit needs it.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waggregate-return"

static void safe_dbm_fetch(DBM *db, datum key, datum *result)
{
    datum temp = dbm_fetch(db, key);
    memcpy(result, &temp, sizeof(datum));
}

static void safe_dbm_firstkey(DBM *db, datum *result)
{
    datum temp = dbm_firstkey(db);
    memcpy(result, &temp, sizeof(datum));
}

static void safe_dbm_nextkey(DBM *db, datum *result)
{
    datum temp = dbm_nextkey(db);
    memcpy(result, &temp, sizeof(datum));
}

#pragma GCC diagnostic pop

/* ------------------------------------------------------------------ */
/* Lifecycle                                                            */
/* ------------------------------------------------------------------ */

int db_open(DbHandle *out)
{
    DBM  *d;
    char *filename;

    if(!out)
    {
        return -1;
    }

    filename = strdup(DATABASE_NAME);

    if(filename == NULL)
    {
        return -1;
    }

    d = dbm_open(filename, O_RDWR | O_CREAT, DATABASE_MODE);
    if(!d)
    {
        free(filename);
        return -1;
    }

    free(filename);
    out->dbm = (void *)d;
    return 0;
}

void db_close(DbHandle *h)
{
    if(h && h->dbm)
    {
        dbm_close((DBM *)h->dbm);
        h->dbm = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* CRUD                                                                 */
/* ------------------------------------------------------------------ */

int db_store(DbHandle *h, const void *key, size_t key_len, const void *val, size_t val_len)
{
    datum k;
    datum v;

    if(!h || !h->dbm || !key || key_len == 0 || !val)
    {
        return -1;
    }

    /* ndbm's dptr is char* / void* depending on the platform; the cast is
     * intentional — we are not modifying the data through this pointer. */
    k.dptr  = (char *)(uintptr_t)key;    // NOLINT(performance-no-int-to-ptr)
    k.dsize = (int)key_len;
    v.dptr  = (char *)(uintptr_t)val;    // NOLINT(performance-no-int-to-ptr)
    v.dsize = (int)val_len;

    return (dbm_store((DBM *)h->dbm, k, v, DBM_REPLACE) == 0) ? 0 : -1;
}

int db_fetch(DbHandle *h, const void *key, size_t key_len, void **val_out, size_t *val_len_out)
{
    datum k;
    datum v;

    if(!h || !h->dbm || !key || key_len == 0)
    {
        return -1;
    }

    k.dptr  = (char *)(uintptr_t)key;    // NOLINT(performance-no-int-to-ptr)
    k.dsize = (int)key_len;

    safe_dbm_fetch((DBM *)h->dbm, k, &v);
    if(!v.dptr)
    {
        return 0; /* not found */
    }

    if(val_out)
    {
        *val_out = (void *)v.dptr;
    }
    if(val_len_out)
    {
        *val_len_out = (size_t)v.dsize;
    }
    return 1;
}

int db_delete(DbHandle *h, const void *key, size_t key_len)
{
    datum k;

    if(!h || !h->dbm || !key || key_len == 0)
    {
        return -1;
    }

    k.dptr  = (char *)(uintptr_t)key;    // NOLINT(performance-no-int-to-ptr)
    k.dsize = (int)key_len;

    return (dbm_delete((DBM *)h->dbm, k) == 0) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Iteration                                                            */
/* ------------------------------------------------------------------ */

int db_first(DbHandle *h, void **key_out, size_t *key_len_out)
{
    datum k;

    if(!h || !h->dbm)
    {
        return -1;
    }

    safe_dbm_firstkey((DBM *)h->dbm, &k);
    if(!k.dptr)
    {
        return 0;
    }

    if(key_out)
    {
        *key_out = (void *)k.dptr;
    }
    if(key_len_out)
    {
        *key_len_out = (size_t)k.dsize;
    }
    return 1;
}

int db_next(DbHandle *h, void **key_out, size_t *key_len_out)
{
    datum k;

    if(!h || !h->dbm)
    {
        return -1;
    }

    safe_dbm_nextkey((DBM *)h->dbm, &k);
    if(!k.dptr)
    {
        return 0;
    }

    if(key_out)
    {
        *key_out = (void *)k.dptr;
    }
    if(key_len_out)
    {
        *key_len_out = (size_t)k.dsize;
    }
    return 1;
}
