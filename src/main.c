#include "arguments.h"
#include "context.h"
#include "errors.h"
#include "signal_util.h"
#include "socket_util.h"
#include "util.h"
#include "watcher.h"
#include "worker.h"
#include <ctype.h>
#include <fcntl.h>
#include <p101_c/p101_stdlib.h>
#include <p101_c/p101_string.h>
#include <p101_convert/integer.h>
#include <p101_fsm/fsm.h>
#include <p101_posix/p101_unistd.h>
#include <p101_posix/sys/p101_socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

enum states
{
    PARSE_ARGS = P101_FSM_USER_START,
    HANDLE_ARGS,
    CREATE_WATCHER,
    CREATE_SOCKET,
    WAIT_FOR_CONNECTION,
    PASS_TO_WORKER,
    USAGE,
    CLEANUP,
    WCHR_SETUP,
};

static void set_argument(const struct p101_env *env, struct p101_error *err, const char **target, const char *value, const char *opt_name);

static p101_fsm_state_t parse_args(const struct p101_env *env, struct p101_error *err, void *context);

static p101_fsm_state_t handle_args(const struct p101_env *env, struct p101_error *err, void *context);

static p101_fsm_state_t create_watcher(const struct p101_env *env, struct p101_error *err, void *context);

static p101_fsm_state_t open_socket(const struct p101_env *env, struct p101_error *err, void *context);

static p101_fsm_state_t wait_for_connection(const struct p101_env *env, struct p101_error *err, void *context);

static p101_fsm_state_t pass_to_worker(const struct p101_env *env, struct p101_error *err, void *context);

static p101_fsm_state_t cleanup(const struct p101_env *env, struct p101_error *err, void *context);

static p101_fsm_state_t usage(const struct p101_env *env, struct p101_error *err, void *context);

#define ERR_MSG_LEN 256    // NOLINT(cppcoreguidelines-macro-to-enum, modernize-macro-to-enum)

#define DEFAULT_CHILDREN 5                  // NOLINT(cppcoreguidelines-macro-to-enum, modernize-macro-to-enum)
#define DEFAULT_PORT 8000                   // NOLINT(cppcoreguidelines-macro-to-enum, modernize-macro-to-enum)
#define DEFAULT_IP "0.0.0.0"                // NOLINT(cppcoreguidelines-macro-to-enum, modernize-macro-to-enum)
#define DEFAULT_SEMAPHORE "/assign3-sem"    // NOLINT(cppcoreguidelines-macro-to-enum, modernize-macro-to-enum)

int main(int argc, char *argv[])
{
    static struct p101_fsm_transition transitions[] = {
        {P101_FSM_INIT,       PARSE_ARGS,          parse_args         },
        {PARSE_ARGS,          HANDLE_ARGS,         handle_args        },
        {PARSE_ARGS,          USAGE,               usage              },
        {PARSE_ARGS,          CLEANUP,             cleanup            },
        {HANDLE_ARGS,         CREATE_WATCHER,      create_watcher     },
        {HANDLE_ARGS,         USAGE,               usage              },
        {HANDLE_ARGS,         CLEANUP,             cleanup            },
        {CREATE_WATCHER,      CREATE_SOCKET,       open_socket        },
        {CREATE_WATCHER,      WCHR_SETUP,          create_watcher     },
        {CREATE_WATCHER,      CLEANUP,             cleanup            },
        {CREATE_SOCKET,       WAIT_FOR_CONNECTION, wait_for_connection},
        {CREATE_SOCKET,       CLEANUP,             cleanup            },
        {WAIT_FOR_CONNECTION, PASS_TO_WORKER,      pass_to_worker     },
        {WAIT_FOR_CONNECTION, CLEANUP,             cleanup            },
        {PASS_TO_WORKER,      WAIT_FOR_CONNECTION, wait_for_connection},
        {PASS_TO_WORKER,      CLEANUP,             cleanup            },
        {USAGE,               CLEANUP,             cleanup            },
        {CLEANUP,             P101_FSM_EXIT,       NULL               }
    };

    struct p101_error    *err;
    struct p101_env      *env;
    struct p101_fsm_info *fsm;
    p101_fsm_state_t      from_state;
    p101_fsm_state_t      to_state;
    struct p101_error    *fsm_err;
    struct p101_env      *fsm_env;
    struct arguments      args;
    struct context        ctx;

    setup_signal_handlers();

    err = p101_error_create(false);

    if(err == NULL)
    {
        ctx.exit_code = EXIT_FAILURE;
        goto done;
    }

    env = p101_env_create(err, true, NULL);

    if(p101_error_has_error(err))
    {
        ctx.exit_code = EXIT_FAILURE;
        goto free_error;
    }

    fsm_err = p101_error_create(false);

    if(fsm_err == NULL)
    {
        ctx.exit_code = EXIT_FAILURE;
        goto free_env;
    }

    fsm_env = p101_env_create(err, true, NULL);

    if(p101_error_has_error(err))
    {
        ctx.exit_code = EXIT_FAILURE;
        goto free_fsm_error;
    }

    p101_memset(env, &args, 0, sizeof(args));
    p101_memset(env, &ctx, 0, sizeof(ctx));
    ctx.arguments       = &args;
    ctx.arguments->argc = argc;
    ctx.arguments->argv = argv;
    ctx.exit_code       = EXIT_SUCCESS;

    // p101_env_set_tracer(env, p101_env_default_tracer);

    fsm = p101_fsm_info_create(env, err, "fsized-fsm", fsm_env, fsm_err, NULL);

    // p101_fsm_info_set_did_change_state_notifier(fsm, p101_fsm_info_default_did_change_state_notifier);
    // p101_fsm_info_set_will_change_state_notifier(fsm, p101_fsm_info_default_will_change_state_notifier);

    p101_fsm_run(fsm, &from_state, &to_state, &ctx, transitions, sizeof(transitions));
    p101_fsm_info_destroy(env, &fsm);

    free(fsm_env);

free_fsm_error:
    p101_error_reset(fsm_err);
    p101_free(env, fsm_err);

free_env:
    p101_free(env, env);

free_error:
    p101_error_reset(err);
    free(err);

done:
    return ctx.exit_code;
}

static p101_fsm_state_t parse_args(const struct p101_env *env, struct p101_error *err, void *context)
{
    struct context  *ctx;
    p101_fsm_state_t next_state;
    int              opt;

    P101_TRACE(env);
    ctx                          = (struct context *)context;
    ctx->arguments->program_name = ctx->arguments->argv[0];
    next_state                   = HANDLE_ARGS;
    opterr                       = 0;

    while((opt = p101_getopt(env, ctx->arguments->argc, ctx->arguments->argv, ":hr:i:p:c:s:")) != -1 && p101_error_has_no_error(err))
    {
        switch(opt)
        {
            case 'h':
                next_state = USAGE;
                break;
            case 'p':
                set_argument(env, err, &ctx->arguments->raw_port, optarg, "p");
                break;
            case 'i':
                set_argument(env, err, &ctx->arguments->raw_ip, optarg, "i");
                break;
            case 'r':
                set_argument(env, err, &ctx->arguments->raw_source, optarg, "r");
                break;
            case 'c':
                set_argument(env, err, &ctx->arguments->raw_children, optarg, "c");
                break;
            case 's':
                set_argument(env, err, &ctx->arguments->raw_semaphore, optarg, "s");
                break;

            case ':':
            {
                char msg[ERR_MSG_LEN];
                snprintf(msg, sizeof msg, "Option '-%c' requires an argument.", optopt ? optopt : '?');
                P101_ERROR_RAISE_USER(err, msg, ERR_USAGE);
                break;
            }
            case '?':
            {
                char msg[ERR_MSG_LEN];
                if(isprint(optopt))
                {
                    snprintf(msg, sizeof msg, "Unknown option '-%c'.", optopt);
                }
                else
                {
                    snprintf(msg, sizeof msg, "Unknown option character 0x%02X.", (unsigned)(unsigned char)optopt);
                }

                P101_ERROR_RAISE_USER(err, msg, ERR_USAGE);
                break;
            }
            default:
            {
                char msg[ERR_MSG_LEN];
                snprintf(msg, sizeof msg, "Internal error: unhandled option '-%c'.", isprint(opt) ? opt : '?');
                P101_ERROR_RAISE_USER(err, msg, ERR_USAGE);
                break;
            }
        }
    }

    // Final Validation
    if(p101_error_has_no_error(err) && next_state != USAGE)
    {
        if(optind != ctx->arguments->argc)
        {
            P101_ERROR_RAISE_USER(err, "Too many unnamed arguments", ERR_USAGE);
        }
    }

    // State Transition Logic
    if(p101_error_is_error(err, P101_ERROR_USER, ERR_USAGE))
    {
        next_state = USAGE;
    }
    else if(p101_error_has_error(err))
    {
        next_state = CLEANUP;
    }

    return next_state;
}

static void set_argument(const struct p101_env *env, struct p101_error *err, const char **target, const char *value, const char *opt_name)
{
    P101_TRACE(env);

    if(*target != NULL)
    {
        char msg[ERR_MSG_LEN];
        snprintf(msg, sizeof(msg), "Option -%s specified more than once.", opt_name);
        P101_ERROR_RAISE_USER(err, msg, ERR_USAGE);
    }
    else
    {
        *target = value;
    }
}

p101_fsm_state_t handle_args(const struct p101_env *env, struct p101_error *err, void *context)
{
    struct context  *ctx;
    p101_fsm_state_t next_state;
    int              addr_status;

    P101_TRACE(env);
    ctx        = (struct context *)context;
    next_state = CREATE_WATCHER;

    ctx->client_socket_fd = -1;
    ctx->client_con_fd    = -1;
    ctx->worker_socket_fd = -1;

    ctx->lib_path = realpath("./libhttp.so", NULL);
    if(!ctx->lib_path)
    {
        P101_ERROR_RAISE_USER(err, "Failed to find libhttp.so", ERR_SYSTEM);
        goto done;
    }

    if(ctx->arguments->raw_source != NULL)
    {
        p101_chdir(env, err, ctx->arguments->raw_source);
        if(p101_error_has_error(err))
        {
            p101_error_reset(err);
            P101_ERROR_RAISE_USER(err, "Invalid root directory path", ERR_USAGE);
            goto done;
        }
    }
    else
    {
        P101_ERROR_RAISE_USER(err, "Root directory (-r) is required", ERR_USAGE);
        goto done;
    }

    if(ctx->arguments->raw_port == NULL)
    {
        ctx->port = DEFAULT_PORT;
    }
    else
    {
        ctx->port = p101_parse_uint16_t(env, err, ctx->arguments->raw_port, DEFAULT_PORT);
        if(p101_error_has_error(err))
        {
            p101_error_reset(err);
            P101_ERROR_RAISE_USER(err, "Invalid port number format", ERR_USAGE);
            goto done;
        }
    }

    if(ctx->arguments->raw_ip == NULL)
    {
        addr_status = convert_address(env, err, DEFAULT_IP, &ctx->addr);
    }
    else
    {
        addr_status = convert_address(env, err, ctx->arguments->raw_ip, &ctx->addr);
    }

    if(addr_status != 0)
    {
        P101_ERROR_RAISE_USER(err, "Failed to parse ip address", ERR_USAGE);
    }

    if(ctx->arguments->raw_children == NULL)
    {
        ctx->children = DEFAULT_CHILDREN;
    }
    else
    {
        ctx->children = p101_parse_uint8_t(env, err, ctx->arguments->raw_children, DEFAULT_CHILDREN);
        if(p101_error_has_error(err))
        {
            p101_error_reset(err);
            P101_ERROR_RAISE_USER(err, "Invalid number of children", ERR_USAGE);
            goto done;
        }
    }

    if(ctx->arguments->raw_semaphore == NULL)
    {
        ctx->semaphore = DEFAULT_SEMAPHORE;
    }
    else
    {
        ctx->semaphore = ctx->arguments->raw_semaphore;
    }

done:
    if(p101_error_is_error(err, P101_ERROR_USER, ERR_USAGE))
    {
        next_state = USAGE;
    }
    else if(p101_error_has_error(err))
    {
        next_state = CLEANUP;
    }

    return next_state;
}

static p101_fsm_state_t create_watcher(const struct p101_env *env, struct p101_error *err, void *context)
{
    struct context  *ctx;
    p101_fsm_state_t next_state;
    int              sv[2];
    pid_t            watcher_pid;

    P101_TRACE(env);
    ctx        = (struct context *)context;
    next_state = CREATE_SOCKET;

    p101_socketpair(env, err, AF_UNIX, SOCK_STREAM, 0, sv);

    if(p101_error_has_error(err))
    {
        goto done;
    }

    watcher_pid = p101_fork(env, err);

    if(p101_error_has_error(err))
    {
        goto done;
    }

    if(watcher_pid == 0)
    {
        p101_close(env, err, sv[0]);
        run_watcher_fsm(env, err, ctx->children, ctx->semaphore, sv[1], ctx->lib_path);
        P101_ERROR_RAISE_USER(err, "Watcher returned", ERR_USAGE);
        goto done;
    }
    else
    {
        p101_close(env, err, sv[1]);
        ctx->watcher_pid      = watcher_pid;
        ctx->worker_socket_fd = sv[0];
    }

done:
    if(p101_error_has_error(err))
    {
        next_state = CLEANUP;
    }

    return next_state;
}

static p101_fsm_state_t open_socket(const struct p101_env *env, struct p101_error *err, void *context)
{
    struct context  *ctx;
    p101_fsm_state_t next_state;

    P101_TRACE(env);
    ctx        = (struct context *)context;
    next_state = WAIT_FOR_CONNECTION;

    ctx->client_socket_fd = create_socket(env, err, ctx->addr.ss_family);

    if(p101_error_has_error(err))
    {
        goto done;
    }

    socket_bind(env, err, ctx->client_socket_fd, &ctx->addr, ctx->port);

    if(p101_error_has_error(err))
    {
        goto done;
    }

    p101_listen(env, err, ctx->client_socket_fd, SOMAXCONN);

    if(p101_error_has_error(err))
    {
        goto done;
    }

    printf("Server listening on: ");
    print_socket(env, err, ctx->client_socket_fd);
    putc('\n', stdout);

done:
    if(p101_error_has_error(err))
    {
        next_state = CLEANUP;
    }

    return next_state;
}

static p101_fsm_state_t wait_for_connection(const struct p101_env *env, struct p101_error *err, void *context)
{
    struct context  *ctx;
    p101_fsm_state_t next_state;

    P101_TRACE(env);
    ctx        = (struct context *)context;
    next_state = PASS_TO_WORKER;

    P101_TRACE(env);

    if(exit_flag == 1)
    {
        if(p101_error_is_errno(err, EINTR))
        {
            p101_error_reset(err);
        }
        next_state = CLEANUP;
        goto done;
    }

    ctx->client_con_fd = p101_accept(env, err, ctx->client_socket_fd, NULL, NULL);

    if(exit_flag == 1)
    {
        if(p101_error_is_errno(err, EINTR))
        {
            p101_error_reset(err);
        }
        next_state = CLEANUP;
    }
    else if(p101_error_has_error(err))
    {
        next_state = CLEANUP;
    }

done:
    return next_state;
}

static p101_fsm_state_t pass_to_worker(const struct p101_env *env, struct p101_error *err, void *context)
{
    struct context  *ctx;
    p101_fsm_state_t next_state;
    struct msghdr    msg;
    struct iovec     iov;
    char             control[CMSG_SPACE(sizeof(int))];
    struct cmsghdr  *cmsg;
    char             dummy_data = 'F';
    int              sent       = 0;

    P101_TRACE(env);
    ctx        = (struct context *)context;
    next_state = WAIT_FOR_CONNECTION;

    // 1. Setup the message (Boilerplate)
    p101_memset(env, &msg, 0, sizeof(msg));
    p101_memset(env, control, 0, sizeof(control));
    iov.iov_base       = &dummy_data;
    iov.iov_len        = sizeof(dummy_data);
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = control;
    msg.msg_controllen = sizeof(control);

    cmsg             = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type  = SCM_RIGHTS;
    cmsg->cmsg_len   = CMSG_LEN(sizeof(int));
    p101_memcpy(env, CMSG_DATA(cmsg), &ctx->client_con_fd, sizeof(int));

    while(!sent)
    {
        if(exit_flag == 1)
        {
            if(p101_error_is_errno(err, EINTR))
            {
                p101_error_reset(err);
            }
            next_state = CLEANUP;
            goto done;
        }

        if(sendmsg(ctx->worker_socket_fd, &msg, 0) != -1)
        {
            sent = 1;    // Success!
            continue;
        }

        if(exit_flag == 1)
        {
            if(p101_error_is_errno(err, EINTR))
            {
                p101_error_reset(err);
            }
            next_state = CLEANUP;
            goto done;
        }

        if(errno == EPIPE || errno == ECONNRESET)
        {
            if(kill(ctx->watcher_pid, 0) == 0)
            {
                printf("Parent: Worker is dead. Waiting for system to provide new socket...\n");
                continue;
            }
            P101_ERROR_RAISE_SYSTEM(err, "sendmsg failed and watcher is dead", errno);
            next_state = CLEANUP;
            goto done;
        }

        P101_ERROR_RAISE_SYSTEM(err, "sendmsg failed fatally", errno);
        next_state = CLEANUP;
        goto done;
    }

    printf("Parent: FD %d passed successfully.\n", ctx->client_con_fd);
    p101_close(env, err, ctx->client_con_fd);
    ctx->client_con_fd = -1;

done:
    return next_state;
}

static p101_fsm_state_t usage(const struct p101_env *env, struct p101_error *err, void *context)
{
    struct context *ctx;

    P101_TRACE(env);
    ctx = (struct context *)context;

    if(p101_error_has_error(err))
    {
        const char *msg;
        msg = p101_error_get_message(err);

        if(msg != NULL)
        {
            fputs(msg, stderr);
            fputc('\n', stderr);
        }

        p101_error_reset(err);
        ctx->exit_code = EXIT_FAILURE;
    }

    fprintf(stderr, "Usage: %s [-s <root_dir>] [-h] [-p <port>] [-i <ip>] \n", ctx->arguments->program_name);
    fputs("Options:\n", stderr);
    fputs("  -h                Display this help message and exit\n", stderr);
    fputs("  -s <root_dir>     Directory to give to clients on connection (default HOME)\n", stderr);
    fputs("  -p <port>         Port to host on (default 8000)\n", stderr);
    fputs("  -i <ip>           Ip to host on (default 0.0.0.0)\n", stderr);
    fputs("  -c <max_clients>  Max number of allow connected clients (default 5, max 255)\n", stderr);

    return CLEANUP;
}

static p101_fsm_state_t cleanup(const struct p101_env *env, struct p101_error *err, void *context)
{
    struct context *ctx;

    P101_TRACE(env);
    ctx = (struct context *)context;

    kill(-getpid(), SIGINT);

    free(ctx->lib_path);

    if(p101_error_has_error(err))
    {
        p101_error_default_error_reporter(err);
        p101_error_reset(err);
        ctx->exit_code = EXIT_FAILURE;
    }

    if(ctx->client_socket_fd >= 0 && p101_close(env, err, ctx->client_socket_fd) == -1)
    {
        fputs(p101_error_get_message(err), stderr);
        fputc('\n', stderr);
        p101_error_reset(err);
    }
    else
    {
        ctx->client_socket_fd = -1;
    }

    if(ctx->worker_socket_fd >= 0 && p101_close(env, err, ctx->worker_socket_fd) == -1)
    {
        fputs(p101_error_get_message(err), stderr);
        fputc('\n', stderr);
        p101_error_reset(err);
    }
    else
    {
        ctx->worker_socket_fd = -1;
    }

    if(ctx->client_con_fd >= 0 && p101_close(env, err, ctx->client_con_fd) == -1)
    {
        fputs(p101_error_get_message(err), stderr);
        fputc('\n', stderr);
        p101_error_reset(err);
    }
    else
    {
        ctx->client_con_fd = -1;
    }

    return P101_FSM_EXIT;
}