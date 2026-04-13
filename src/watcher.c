#include "watcher.h"
#include "errors.h"
#include "signal_util.h"
#include "worker.h"
#include <errno.h>
#include <p101_c/p101_stdlib.h>
#include <p101_c/p101_string.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

enum wchr_states
{
    WCHR_SETUP = P101_FSM_USER_START,
    WCHR_WATCH,
    WCHR_CLEANUP
};

static p101_fsm_state_t wchr_setup(const struct p101_env *env, struct p101_error *err, void *context);
static p101_fsm_state_t wchr_watch(const struct p101_env *env, struct p101_error *err, void *context);
static p101_fsm_state_t wchr_cleanup(const struct p101_env *env, struct p101_error *err, void *context);

noreturn void run_watcher_fsm(const struct p101_env *env, struct p101_error *err, uint8_t children, const char *semaphore, const int parent_fd, char *lib_path)
{
    const struct p101_fsm_transition wchr_transitions[] = {
        {P101_FSM_INIT, WCHR_SETUP,    wchr_setup  },
        {WCHR_SETUP,    WCHR_WATCH,    wchr_watch  },
        {WCHR_SETUP,    WCHR_CLEANUP,  wchr_cleanup},
        {WCHR_WATCH,    WCHR_CLEANUP,  wchr_cleanup},
        {WCHR_SETUP,    WCHR_CLEANUP,  wchr_cleanup},
        {WCHR_CLEANUP,  P101_FSM_EXIT, NULL        }
    };

    struct wchr_context   ctx;
    struct p101_fsm_info *fsm;
    p101_fsm_state_t      from_state;
    p101_fsm_state_t      to_state;

    P101_TRACE(env);
    p101_memset(env, &ctx, 0, sizeof(ctx));

    ctx.children  = children;
    ctx.semaphore = semaphore;
    ctx.parent_fd = parent_fd;
    ctx.lib_path  = lib_path;
    ctx.exit_code = EXIT_SUCCESS;

    fsm = p101_fsm_info_create(env, err, "watcher-fsm", NULL, NULL, NULL);

    if(p101_error_has_no_error(err))
    {
        p101_fsm_run(fsm, &from_state, &to_state, &ctx, wchr_transitions, sizeof(wchr_transitions));
        p101_fsm_info_destroy(env, &fsm);
    }

    _exit(ctx.exit_code);
}

static p101_fsm_state_t wchr_setup(const struct p101_env *env, struct p101_error *err, void *context)
{
    const struct wchr_context *ctx;
    p101_fsm_state_t           next_state;

    P101_TRACE(env);
    ctx        = (struct wchr_context *)context;
    next_state = WCHR_WATCH;

    for(uint32_t i = 0; i < ctx->children; i++)
    {
        pid_t pid = fork();

        if(pid == -1)
        {
            P101_ERROR_RAISE_SYSTEM(err, "A Watcher fork failed", errno);
            break;
        }

        if(pid == 0)
        {
            run_worker_fsm(env, err, ctx->semaphore, ctx->parent_fd, ctx->lib_path);
            P101_ERROR_RAISE_USER(err, "A Worker returned", ERR_USAGE);
            next_state = WCHR_CLEANUP;
        }
    }

    return next_state;
}

static p101_fsm_state_t wchr_watch(const struct p101_env *env, struct p101_error *err, void *context)
{
    const struct wchr_context *ctx;

    P101_TRACE(env);
    ctx = (struct wchr_context *)context;

    while(exit_flag != 1)
    {
        pid_t exited_pid;
        pid_t pid;

        exited_pid = wait(NULL);

        if(exit_flag == 1)
        {
            break;
        }

        if(exited_pid == -1)
        {
            P101_ERROR_RAISE_SYSTEM(err, "wait failed", errno);
            break;
        }

        pid = fork();

        if(pid == -1)
        {
            P101_ERROR_RAISE_SYSTEM(err, "A Watcher fork failed", errno);
            break;
        }

        if(pid == 0)
        {
            run_worker_fsm(env, err, ctx->semaphore, ctx->parent_fd, ctx->lib_path);
            P101_ERROR_RAISE_USER(err, "A Worker returned", ERR_USAGE);
        }

        sleep(3);
    }

    return WCHR_CLEANUP;
}

static p101_fsm_state_t wchr_cleanup(const struct p101_env *env, struct p101_error *err, void *context)
{
    struct wchr_context *ctx;

    P101_TRACE(env);
    ctx = (struct wchr_context *)context;

    free(ctx->lib_path);

    kill(-getpid(), SIGINT);

    if(p101_error_has_error(err))
    {
        p101_error_default_error_reporter(err);
        p101_error_reset(err);
        ctx->exit_code = EXIT_FAILURE;
    }

    return P101_FSM_EXIT;
}
