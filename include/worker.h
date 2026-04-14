#ifndef WORKER_H
#define WORKER_H

#include <semaphore.h>
#include <stdbool.h>
#include <stdnoreturn.h>

#include <p101_fsm/fsm.h>

#include "http.h"

/* ------------------------------------------------------------------ */
/* Default shared-library path; override at compile time if needed     */
/* cc ... -DHTTP_LIB_PATH=\"/opt/myapp/libhttp.so\"                    */
/* ------------------------------------------------------------------ */
#define HTTP_LIB_PATH "libhttp.so"
#define PATH_MAX 8192 // NOLINT(cppcoreguidelines-macro-to-enum, modernize-macro-to-enum)
#define DLOPEN_BUF 65536 // NOLINT(cppcoreguidelines-macro-to-enum, modernize-macro-to-enum)
#define DLOPEN_FLAGS 0700 // NOLINT(cppcoreguidelines-macro-to-enum, modernize-macro-to-enum)

/* ------------------------------------------------------------------ */
/* Function-pointer typedefs matching the http.h public API            */
/* ------------------------------------------------------------------ */
typedef int  (*fn_read_request)    (int,          HttpRequest *);
typedef bool (*fn_validate_request)(HttpRequest *, HttpResponse *);
typedef int  (*fn_handle_operation)(HttpRequest *, HttpResponse *, sem_t *);
typedef int  (*fn_send_response)   (int,          HttpResponse *);
typedef void (*fn_free_request)    (HttpRequest *);
typedef int (*fn_version_request)    (void);

/* ------------------------------------------------------------------ */
/* Per-worker FSM context                                               */
/* ------------------------------------------------------------------ */
struct wkr_context
{
    /* --- Provided at construction ---------------------------------- */
    const char *semaphore;   /* POSIX semaphore name, e.g. "/http_db" */
    int         parent_fd;   /* Unix-socket fd for SCM_RIGHTS from parent */
    int         client_fd;   /* Accepted client fd; -1 when none held */
    int         exit_code;   /* Propagated to _exit() at FSM end      */

    /* --- Dynamic library (loaded in wkr_check_lib) ----------------- */
    void               *lib_handle;   /* dlopen() handle              */
    fn_read_request     fn_read;
    fn_validate_request fn_validate;
    fn_handle_operation fn_handle;
    fn_send_response    fn_send;
    fn_free_request     fn_free;
    fn_version_request  fn_version;

    /* --- POSIX semaphore (sem_open'd in wkr_setup) ----------------- */
    sem_t *db_sem;           /* Passed into the library via its extern */

    char  *abs_lib_path; // Store the "real" path here
    time_t last_mtime;

    /* --- Per-request state (lives across wkr_read → wkr_respond) -- */
    HttpRequest  req;
    HttpResponse res;
    bool         req_active; /* true  → req holds data, must be freed  */
};

/* ------------------------------------------------------------------ */
/* Public entry point                                                   */
/* ------------------------------------------------------------------ */
noreturn void run_worker_fsm(const struct p101_env *env,
                    struct p101_error     *err,
                    const char            *semaphore,
                    int                    parent_fd,
                    char                  *lib_path);

#endif /* WORKER_H */
