#include "worker.h"
#include "context.h"
#include "errors.h"
#include "signal_util.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <p101_c/p101_stdlib.h>
#include <p101_c/p101_string.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* FSM state IDs                                                        */
/* ------------------------------------------------------------------ */

enum wkr_states
{
    WKR_SETUP = P101_FSM_USER_START,
    WKR_CHECK_LIB,
    WKR_WAIT_FOR_CLIENT,
    WKR_READ_REQUEST,
    WKR_PARSE,
    WKR_RESPOND,
    WKR_CLEANUP
};

/* ------------------------------------------------------------------ */
/* Forward declarations                                                 */
/* ------------------------------------------------------------------ */

static p101_fsm_state_t wkr_setup(const struct p101_env *env, struct p101_error *err, void *context);
static p101_fsm_state_t wkr_check_lib(const struct p101_env *env, struct p101_error *err, void *context);
static p101_fsm_state_t wkr_wait_for_client(const struct p101_env *env, struct p101_error *err, void *context);
static p101_fsm_state_t wkr_read_request(const struct p101_env *env, struct p101_error *err, void *context);
static p101_fsm_state_t wkr_parse(const struct p101_env *env, struct p101_error *err, void *context);
static p101_fsm_state_t wkr_respond(const struct p101_env *env, struct p101_error *err, void *context);
static p101_fsm_state_t wkr_cleanup(const struct p101_env *env, struct p101_error *err, void *context);

static void *dlopen_fresh(const char *lib_path, int flags);

/* ------------------------------------------------------------------ */
/* Public entry point                                                   */
/* ------------------------------------------------------------------ */

noreturn void run_worker_fsm(const struct p101_env *env, struct p101_error *err, const char *semaphore, const int parent_fd, char *lib_path)
{
    struct wkr_context    ctx;
    struct p101_fsm_info *fsm;
    p101_fsm_state_t      from_state;
    p101_fsm_state_t      to_state;

    const struct p101_fsm_transition wkr_transitions[] = {
        {P101_FSM_INIT,       WKR_SETUP,           wkr_setup          },
        {WKR_SETUP,           WKR_CHECK_LIB,       wkr_check_lib      },
        {WKR_SETUP,           WKR_CLEANUP,         wkr_cleanup        },
        {WKR_CHECK_LIB,       WKR_WAIT_FOR_CLIENT, wkr_wait_for_client},
        {WKR_CHECK_LIB,       WKR_CLEANUP,         wkr_cleanup        },
        {WKR_WAIT_FOR_CLIENT, WKR_READ_REQUEST,    wkr_read_request   },
        {WKR_WAIT_FOR_CLIENT, WKR_CLEANUP,         wkr_cleanup        },
        {WKR_READ_REQUEST,    WKR_PARSE,           wkr_parse          },
        {WKR_READ_REQUEST,    WKR_CHECK_LIB,       wkr_check_lib      },
        {WKR_READ_REQUEST,    WKR_CLEANUP,         wkr_cleanup        },
        {WKR_PARSE,           WKR_RESPOND,         wkr_respond        },
        {WKR_PARSE,           WKR_CLEANUP,         wkr_cleanup        },
        {WKR_RESPOND,         WKR_READ_REQUEST,    wkr_read_request   },
        {WKR_RESPOND,         WKR_CHECK_LIB,       wkr_check_lib      },
        {WKR_RESPOND,         WKR_CLEANUP,         wkr_cleanup        },
        {WKR_CLEANUP,         P101_FSM_EXIT,       NULL               }
    };

    p101_memset(env, &ctx, 0, sizeof(struct wkr_context));
    ctx.semaphore    = semaphore;
    ctx.parent_fd    = parent_fd;
    ctx.client_fd    = -1;
    ctx.db_sem       = NULL;
    ctx.lib_handle   = NULL;
    ctx.abs_lib_path = lib_path;
    ctx.req_active   = false;
    ctx.res.file_fd  = -1;
    ctx.exit_code    = EXIT_SUCCESS;

    fsm = p101_fsm_info_create(env, err, "worker-fsm", env, err, NULL);

    if(p101_error_has_no_error(err))
    {
        p101_fsm_run(fsm, &from_state, &to_state, &ctx, wkr_transitions, sizeof(wkr_transitions));
        p101_fsm_info_destroy(env, &fsm);
    }

    _exit(ctx.exit_code);
}

/* ------------------------------------------------------------------ */
/* WKR_SETUP                                                            */
/* ------------------------------------------------------------------ */

static p101_fsm_state_t wkr_setup(const struct p101_env *env, struct p101_error *err, void *context)
{
    struct wkr_context *w_ctx;

    P101_TRACE(env);
    w_ctx = (struct wkr_context *)context;
    p101_error_reset(err);

    w_ctx->db_sem = sem_open(w_ctx->semaphore, O_CREAT);
    if(w_ctx->db_sem == SEM_FAILED)
    {
        w_ctx->db_sem    = NULL;
        w_ctx->exit_code = EXIT_FAILURE;
        P101_ERROR_RAISE_SYSTEM(err, "sem_open failed", errno);
        return WKR_CLEANUP;
    }

    return WKR_CHECK_LIB;
}

/* ------------------------------------------------------------------ */
/* WKR_CHECK_LIB                                                        */
/*                                                                      */
/* Loads the HTTP shared library on first call; skips reload on        */
/* subsequent passes (keep-alive loop or after non-keep-alive close).  */
/* Wires db_sem into the library's extern after loading.               */
/* ------------------------------------------------------------------ */

static p101_fsm_state_t wkr_check_lib(const struct p101_env *env, struct p101_error *err, void *context)
{
    struct wkr_context *w_ctx;
    struct stat         st;
    bool                should_load = false;

    P101_TRACE(env);
    w_ctx = (struct wkr_context *)context;
    p101_error_reset(err);

    if(exit_flag == 1)
    {
        return WKR_CLEANUP;
    }

    if(stat(w_ctx->abs_lib_path, &st) != 0)
    {
        fprintf(stderr, "Worker [%d]: stat '%s' failed: %s\n", getpid(), w_ctx->abs_lib_path, strerror(errno));
        P101_ERROR_RAISE_USER(err, "stat failed", ERR_SYSTEM);
        return WKR_CLEANUP;
    }

    if(w_ctx->lib_handle == NULL || st.st_mtime != w_ctx->last_mtime)
    {
        should_load = true;
    }

    if(!should_load)
    {
        return WKR_WAIT_FOR_CLIENT;
    }

    if(w_ctx->lib_handle != NULL)
    {
        printf("Worker [%d]: Change detected in '%s'. Reloading...\n", getpid(), w_ctx->abs_lib_path);
        dlclose(w_ctx->lib_handle);
        w_ctx->lib_handle = NULL;
    }

    (void)dlerror();
    if(sem_wait(w_ctx->db_sem) != 0)
    {
        P101_ERROR_RAISE_USER(err, "dlopen failed", ERR_SYSTEM);
        return WKR_CLEANUP;
    }
    w_ctx->lib_handle = dlopen_fresh(w_ctx->abs_lib_path, RTLD_NOW | RTLD_LOCAL);
    sem_post(w_ctx->db_sem);
    if(!w_ctx->lib_handle)
    {
        fprintf(stderr, "Worker [%d]: dlopen '%s': %s\n", getpid(), w_ctx->abs_lib_path, dlerror());
        P101_ERROR_RAISE_USER(err, "dlopen failed", ERR_SYSTEM);
        return WKR_CLEANUP;
    }

    w_ctx->last_mtime = st.st_mtime;

    (void)dlerror();

    *(void **)(&w_ctx->fn_read)     = dlsym(w_ctx->lib_handle, "http_read_request");
    *(void **)(&w_ctx->fn_validate) = dlsym(w_ctx->lib_handle, "http_validate_request");
    *(void **)(&w_ctx->fn_handle)   = dlsym(w_ctx->lib_handle, "http_handle_operation");
    *(void **)(&w_ctx->fn_send)     = dlsym(w_ctx->lib_handle, "http_send_response");
    *(void **)(&w_ctx->fn_free)     = dlsym(w_ctx->lib_handle, "http_free_request");
    *(void **)(&w_ctx->fn_version)  = dlsym(w_ctx->lib_handle, "get_version");

    if(!w_ctx->fn_read || !w_ctx->fn_validate || !w_ctx->fn_handle || !w_ctx->fn_send || !w_ctx->fn_free || !w_ctx->fn_version)
    {
        fprintf(stderr, "Worker [%d]: dlsym: %s\n", getpid(), dlerror());
        P101_ERROR_RAISE_USER(err, "dlsym failed for one or more symbols", ERR_SYSTEM);
        dlclose(w_ctx->lib_handle);
        w_ctx->lib_handle = NULL;
        w_ctx->exit_code  = EXIT_FAILURE;
        return WKR_CLEANUP;
    }

    printf("Worker [%d]: library '%s' loaded (mtime: %ld, version: %d).\n", getpid(), w_ctx->abs_lib_path, (long)w_ctx->last_mtime, w_ctx->fn_version());

    return WKR_WAIT_FOR_CLIENT;
}

/**
 * Opens a shared library via a temp-copy so that each load gets a
 * fresh inode, bypassing ld.so's device+inode cache.
 *
 * The temp file is unlinked immediately after dlopen — safe because
 * the returned handle holds a reference to the inode.
 *
 * Returns a valid handle on success, NULL on failure (dlerror() set).
 */
static void *dlopen_fresh(const char *lib_path, int flags)
{
    char    tmp_path[PATH_MAX];
    void   *handle;
    int     in;
    int     out;
    char    buf[DLOPEN_BUF];
    ssize_t n;

    snprintf(tmp_path, sizeof(tmp_path), "%s.reload.tmp", lib_path);

    in  = open(lib_path, O_RDONLY | O_CLOEXEC);
    out = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, DLOPEN_FLAGS);

    if(in < 0 || out < 0)
    {
        fprintf(stderr, "dlopen_fresh: open failed: %s\n", strerror(errno));
        goto err;
    }

    while((n = read(in, buf, sizeof(buf))) > 0)
    {
        if(write(out, buf, (size_t)n) != n)
        {
            fprintf(stderr, "dlopen_fresh: write failed: %s\n", strerror(errno));
            goto err;
        }
    }

    close(in);
    close(out);

    handle = dlopen(tmp_path, flags);
    unlink(tmp_path);
    return handle; /* NULL if dlopen failed; dlerror() is set */

err:
    if(in >= 0)
    {
        close(in);
    }
    if(out >= 0)
    {
        close(out);
    }
    unlink(tmp_path);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* WKR_WAIT_FOR_CLIENT                                                  */
/* ------------------------------------------------------------------ */

static p101_fsm_state_t wkr_wait_for_client(const struct p101_env *env, struct p101_error *err, void *context)
{
    struct wkr_context *w_ctx;
    p101_fsm_state_t    next_state;
    ssize_t             msg_status;
    struct msghdr       msg;
    struct iovec        iov;
    struct cmsghdr     *cmsg;
    char                control[CMSG_SPACE(sizeof(int))];
    char                dummy;

    P101_TRACE(env);
    w_ctx      = (struct wkr_context *)context;
    next_state = WKR_READ_REQUEST;

    w_ctx->client_fd = -1;

    if(exit_flag == 1)
    {
        next_state = WKR_CLEANUP;
        goto done;
    }

    p101_memset(env, &msg, 0, sizeof(msg));
    iov.iov_base       = &dummy;
    iov.iov_len        = sizeof(dummy);
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = control;
    msg.msg_controllen = sizeof(control);

    msg_status = recvmsg(w_ctx->parent_fd, &msg, 0);

    if(msg_status == 0)
    {
        printf("Worker [%d]: parent closed connection, shutting down.\n", getpid());
        next_state = WKR_CLEANUP;
        goto done;
    }

    if(msg_status < 0)
    {
        if(exit_flag == 1)
        {
            p101_error_reset(err);
        }
        else
        {
            P101_ERROR_RAISE_SYSTEM(err, "recvmsg failed", errno);
            w_ctx->exit_code = EXIT_FAILURE;
        }
        next_state = WKR_CLEANUP;
        goto done;
    }

    cmsg = CMSG_FIRSTHDR(&msg);
    if(cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS)
    {
        p101_memcpy(env, &w_ctx->client_fd, CMSG_DATA(cmsg), sizeof(int));
        printf("Worker [%d]: received client fd %d.\n", getpid(), w_ctx->client_fd);
    }
    else
    {
        printf("Worker [%d]: recvmsg carried no file descriptor.\n", getpid());
        P101_ERROR_RAISE_USER(err, "SCM_RIGHTS fd missing", ERR_SYSTEM);
        w_ctx->exit_code = EXIT_FAILURE;
        next_state       = WKR_CLEANUP;
    }

done:
    return next_state;
}

/* ------------------------------------------------------------------ */
/* WKR_READ_REQUEST                                                     */
/*                                                                      */
/* fn_read applies the 10-second socket timeout, reads headers and     */
/* body, and returns:                                                   */
/*    0   success (method may still be UNKNOWN — parse handles it)     */
/*   -1   I/O failure, timeout, or malloc failure                      */
/*                                                                      */
/* -1 is treated as a connection-level problem: close the client fd    */
/* and exit with EXIT_SUCCESS so the parent respawns cleanly.          */
/* ------------------------------------------------------------------ */

static p101_fsm_state_t wkr_read_request(const struct p101_env *env, struct p101_error *err, void *context)
{
    struct wkr_context *w_ctx;
    int                 ret;

    P101_TRACE(env);
    w_ctx = (struct wkr_context *)context;
    p101_error_reset(err);

    p101_memset(env, &w_ctx->req, 0, sizeof(HttpRequest));
    p101_memset(env, &w_ctx->res, 0, sizeof(HttpResponse));
    w_ctx->res.file_fd = -1;
    w_ctx->req_active  = false;

    if(exit_flag == 1)
    {
        return WKR_CLEANUP;
    }

    ret = w_ctx->fn_read(w_ctx->client_fd, &w_ctx->req);
    if(ret < 0)
    {
        /* Timeout, client disconnect, or malloc failure.
         * Not a hard worker error — exit cleanly for parent to respawn. */
        printf("Worker [%d]: closed connection %d (timeout or I/O error).\n", getpid(), w_ctx->client_fd);
        close(w_ctx->client_fd);
        w_ctx->client_fd = -1;
        return WKR_CHECK_LIB;
    }

    printf("Worker [%d]: read request from client.\n", getpid());

    w_ctx->req_active = true;
    return WKR_PARSE;
}

/* ------------------------------------------------------------------ */
/* WKR_PARSE                                                            */
/*                                                                      */
/* fn_validate:  false → res holds 400/501, go to WKR_RESPOND          */
/*               (parse/format error; client gets an HTTP response)    */
/*                                                                      */
/* fn_handle:    0  → res fully populated, go to WKR_RESPOND           */
/*              -1  → hard system error (fstat, malloc); res is        */
/*                    invalid.  Worker exits with EXIT_FAILURE.        */
/* ------------------------------------------------------------------ */

static p101_fsm_state_t wkr_parse(const struct p101_env *env, struct p101_error *err, void *context)
{
    struct wkr_context *w_ctx;
    bool                valid;
    int                 op_ret;

    P101_TRACE(env);
    w_ctx = (struct wkr_context *)context;
    p101_error_reset(err);

    valid = w_ctx->fn_validate(&w_ctx->req, &w_ctx->res);
    if(!valid)
    {
        /* Parse/format/version error.  res already carries 400 or 501.
         * Fall through to WKR_RESPOND so the client receives it. */
        return WKR_RESPOND;
    }

    op_ret = w_ctx->fn_handle(&w_ctx->req, &w_ctx->res, w_ctx->db_sem);
    if(op_ret < 0)
    {
        /* Hard system error (fstat failure, malloc failure).
         * res is not valid — do not attempt to send it. */
        fprintf(stderr, "Worker [%d]: fn_handle hard error, exiting.\n", getpid());
        w_ctx->exit_code = EXIT_FAILURE;
        return WKR_CLEANUP;
    }

    /* fn_handle returned 0: res may carry a 404, 500, 201, etc.
     * All of those are valid HTTP responses — proceed to send. */
    return WKR_RESPOND;
}

/* ------------------------------------------------------------------ */
/* WKR_RESPOND                                                          */
/*                                                                      */
/* fn_send failure is a connection-level error (client gone, timeout). */
/* Not a hard worker error: close the fd and wait for the next client  */
/* via WKR_CHECK_LIB → WKR_WAIT_FOR_CLIENT.                           */
/*                                                                      */
/* Keep-alive:     stay on the same fd → WKR_READ_REQUEST              */
/* Normal close:   release fd → WKR_CHECK_LIB                          */
/* Send failure:   release fd → WKR_CHECK_LIB                          */
/* exit_flag set:  release fd → WKR_CLEANUP                            */
/* ------------------------------------------------------------------ */

static p101_fsm_state_t wkr_respond(const struct p101_env *env, struct p101_error *err, void *context)
{
    struct wkr_context *w_ctx;
    p101_fsm_state_t    next_state;
    int                 send_ret;

    P101_TRACE(env);
    w_ctx = (struct wkr_context *)context;
    p101_error_reset(err);

    send_ret = w_ctx->fn_send(w_ctx->client_fd, &w_ctx->res);

    /* Always free the request after attempting to send */
    if(w_ctx->req_active)
    {
        w_ctx->fn_free(&w_ctx->req);
        w_ctx->req_active = false;
    }

    if(send_ret < 0)
    {
        /* Client disconnected or write timed out.
         * Not a hard error — just get the next client. */
        printf("Worker [%d]: fn_send failed on fd %d, closing connection.\n", getpid(), w_ctx->client_fd);
        close(w_ctx->client_fd);
        w_ctx->client_fd = -1;
        return WKR_CHECK_LIB;
    }

    printf("Worker [%d]: sent response to client.\n", getpid());

    if(exit_flag == 1)
    {
        close(w_ctx->client_fd);
        w_ctx->client_fd = -1;
        return WKR_CLEANUP;
    }

    if(w_ctx->res.keep_alive)
    {
        /* Reuse the open socket: read the next pipelined request */
        next_state = WKR_READ_REQUEST;
    }
    else
    {
        close(w_ctx->client_fd);
        w_ctx->client_fd = -1;
        next_state       = WKR_CHECK_LIB;
    }

    return next_state;
}

/* ------------------------------------------------------------------ */
/* WKR_CLEANUP                                                          */
/*                                                                      */
/* Releases every resource in dependency order:                        */
/*   in-flight request → client fd → library → semaphore              */
/* Safe to call at any point in the FSM; each resource is null-guarded */
/* and zeroed after release.                                            */
/* ------------------------------------------------------------------ */

static p101_fsm_state_t wkr_cleanup(const struct p101_env *env, struct p101_error *err, void *context)
{
    struct wkr_context *w_ctx;

    P101_TRACE(env);
    w_ctx = (struct wkr_context *)context;

    free(w_ctx->abs_lib_path);

    if(p101_error_has_error(err))
    {
        p101_error_default_error_reporter(err);
        p101_error_reset(err);
        w_ctx->exit_code = EXIT_FAILURE;
    }

    if(w_ctx->req_active)
    {
        if(w_ctx->fn_free)
        {
            w_ctx->fn_free(&w_ctx->req);
        }
        w_ctx->req_active = false;
    }

    if(w_ctx->client_fd >= 0)
    {
        close(w_ctx->client_fd);
        w_ctx->client_fd = -1;
    }

    if(w_ctx->lib_handle)
    {
        dlclose(w_ctx->lib_handle);
        w_ctx->lib_handle  = NULL;
        w_ctx->fn_read     = NULL;
        w_ctx->fn_validate = NULL;
        w_ctx->fn_handle   = NULL;
        w_ctx->fn_send     = NULL;
        w_ctx->fn_free     = NULL;
    }

    if(w_ctx->db_sem != SEM_FAILED)
    {
        sem_close(w_ctx->db_sem);
        w_ctx->db_sem = NULL;
    }

    p101_error_reset(err);
    return P101_FSM_EXIT;
}
