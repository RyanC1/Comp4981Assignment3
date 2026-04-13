#include "db_util.h"
#include "errors.h"
#include "signal_util.h"
#include <ctype.h>
#include <p101_c/p101_stdlib.h>
#include <p101_c/p101_string.h>
#include <p101_fsm/fsm.h>
#include <p101_posix/p101_unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ERR_MSG_LEN 256                     // NOLINT(cppcoreguidelines-macro-to-enum, modernize-macro-to-enum)
#define CMD_LEN 16                          // NOLINT(cppcoreguidelines-macro-to-enum, modernize-macro-to-enum)
#define KEY_LEN 1024                        // NOLINT(cppcoreguidelines-macro-to-enum, modernize-macro-to-enum)
#define DEFAULT_SEMAPHORE "/assign3-sem"    // NOLINT(cppcoreguidelines-macro-to-enum, modernize-macro-to-enum)

enum states
{
    PARSE_ARGS = P101_FSM_USER_START,
    HANDLE_ARGS,
    OPEN_RESOURCES,
    COMMAND_LOOP,
    USAGE,
    CLEANUP
};

struct arguments
{
    int         argc;
    const char *program_name;

    const char *raw_db_path;
    const char *raw_sem_path;

    char **argv;
};

struct context
{
    struct arguments *arguments;
    const char       *semaphore;

    DbHandle db;
    int      exit_code;
};

/* ------------------------------------------------------------------ */
/* Static Function Prototypes                                         */
/* ------------------------------------------------------------------ */

static void set_argument(const struct p101_env *env, struct p101_error *err, const char **target, const char *value, const char *opt_name);

static p101_fsm_state_t parse_args(const struct p101_env *env, struct p101_error *err, void *context);
static p101_fsm_state_t handle_args(const struct p101_env *env, struct p101_error *err, void *context);
static p101_fsm_state_t open_resources(const struct p101_env *env, struct p101_error *err, void *context);
static p101_fsm_state_t command_loop(const struct p101_env *env, struct p101_error *err, void *context);
static p101_fsm_state_t usage(const struct p101_env *env, struct p101_error *err, void *context);
static p101_fsm_state_t cleanup(const struct p101_env *env, struct p101_error *err, void *context);

/* ------------------------------------------------------------------ */
/* FSM Implementation                                                 */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    struct p101_error    *err;
    struct p101_env      *env;
    struct p101_fsm_info *fsm;
    struct arguments      args;
    struct context        ctx;
    p101_fsm_state_t      from_state;
    p101_fsm_state_t      to_state;

    setup_signal_handlers();

    err = p101_error_create(false);
    if(!err)
    {
        return EXIT_FAILURE;
    }

    env = p101_env_create(err, true, NULL);
    if(!env)
    {
        free(err);
        return EXIT_FAILURE;
    }

    // CRITICAL: Initialize context and args BEFORE FSM run
    p101_memset(env, &args, 0, sizeof(args));
    p101_memset(env, &ctx, 0, sizeof(ctx));

    p101_memset(env, &args, 0, sizeof(args));
    p101_memset(env, &ctx, 0, sizeof(ctx));
    ctx.arguments       = &args;
    ctx.arguments->argc = argc;
    ctx.arguments->argv = argv;
    ctx.exit_code       = EXIT_SUCCESS;

    // Use the same env/err for the FSM info
    fsm = p101_fsm_info_create(env, err, "db-fsm", env, err, NULL);

    if(fsm)
    {
        static struct p101_fsm_transition transitions[] = {
            {P101_FSM_INIT,  PARSE_ARGS,     parse_args    },
            {PARSE_ARGS,     HANDLE_ARGS,    handle_args   },
            {PARSE_ARGS,     USAGE,          usage         },
            {HANDLE_ARGS,    OPEN_RESOURCES, open_resources},
            {HANDLE_ARGS,    USAGE,          usage         },
            {HANDLE_ARGS,    CLEANUP,        cleanup       },
            {OPEN_RESOURCES, COMMAND_LOOP,   command_loop  },
            {OPEN_RESOURCES, CLEANUP,        cleanup       },
            {COMMAND_LOOP,   CLEANUP,        cleanup       },
            {USAGE,          CLEANUP,        cleanup       },
            {CLEANUP,        P101_FSM_EXIT,  NULL          }
        };

        p101_fsm_run(fsm, &from_state, &to_state, &ctx, transitions, sizeof(transitions));
        p101_fsm_info_destroy(env, &fsm);
    }

    // Cleanup order matters
    p101_free(env, env);
    free(err);    // p101_free needs a valid env, so use standard free for the error if env is gone

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

    while((opt = p101_getopt(env, ctx->arguments->argc, ctx->arguments->argv, ":hd:s:")) != -1 && p101_error_has_no_error(err))
    {
        switch(opt)
        {
            case 'h':
                next_state = USAGE;
                break;
            case 'd':
                set_argument(env, err, &ctx->arguments->raw_db_path, optarg, "p");
                break;
            case 's':
                set_argument(env, err, &ctx->arguments->raw_sem_path, optarg, "s");
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

static p101_fsm_state_t handle_args(const struct p101_env *env, struct p101_error *err, void *context)
{
    struct context *ctx;

    P101_TRACE(env);
    ctx = (struct context *)context;

    if(ctx->arguments->raw_db_path != NULL)
    {
        p101_chdir(env, err, ctx->arguments->raw_db_path);
        if(p101_error_has_error(err))
        {
            p101_error_reset(err);
            P101_ERROR_RAISE_USER(err, "Invalid root directory path", ERR_USAGE);
            goto done;
        }
    }
    else
    {
        P101_ERROR_RAISE_USER(err, "Database directory (-d) is required", ERR_USAGE);
        goto done;
    }

    if(ctx->arguments->raw_sem_path == NULL)
    {
        ctx->semaphore = DEFAULT_SEMAPHORE;
    }
    else
    {
        ctx->semaphore = ctx->arguments->raw_sem_path;
    }

done:
    if(p101_error_has_error(err))
    {
        return USAGE;
    }
    return OPEN_RESOURCES;
}

static p101_fsm_state_t open_resources(const struct p101_env *env, struct p101_error *err, void *context)
{
    struct context *ctx;

    P101_TRACE(env);
    ctx = (struct context *)context;

    if(db_open(&ctx->db) != 0)
    {
        P101_ERROR_RAISE_SYSTEM(err, "db_open failed", errno);
        return CLEANUP;
    }

    return COMMAND_LOOP;
}

static p101_fsm_state_t command_loop(const struct p101_env *env, struct p101_error *err, void *context)
{
    struct context *ctx;
    char            cmd[CMD_LEN];
    char            key_buf[KEY_LEN];
    char            val_buf[KEY_LEN];

    P101_TRACE(env);
    ctx = (struct context *)context;

    while(printf("> ") > 0 && scanf("%15s", cmd) != EOF)
    {
        if(exit_flag == 1)
        {
            break;
        }

        if(cmd[0] == 'q')
        {
            break;
        }

        switch(cmd[0])
        {
            case 's': /* store */
            {
                printf("Key: ");
                if(scanf("%1023s", key_buf) == 1)
                {
                    printf("Value: ");
                    if(scanf("%1023s", val_buf) == 1)
                    {
                        if(db_store(&ctx->db, key_buf, strlen(key_buf), val_buf, strlen(val_buf)) != 0)
                        {
                            P101_ERROR_RAISE_USER(err, "Store operation failed", ERR_SYSTEM);
                        }
                    }
                }
                break;
            }
            case 'f': /* fetch */
            {
                void  *val;
                size_t val_len;

                printf("Key: ");
                if(scanf("%1023s", key_buf) == 1)
                {
                    int rc;
                    rc = db_fetch(&ctx->db, key_buf, strlen(key_buf), &val, &val_len);
                    if(rc == 1)
                    {
                        printf("Value: %.*s\n", (int)val_len, (char *)val);
                    }
                    else if(rc == 0)
                    {
                        printf("Key not found.\n");
                    }
                    else
                    {
                        P101_ERROR_RAISE_USER(err, "Fetch operation failed", ERR_SYSTEM);
                    }
                }
                break;
            }
            case 'l': /* list */
            {
                void  *key;
                size_t key_len;
                int    r;

                for(r = db_first(&ctx->db, &key, &key_len); r == 1; r = db_next(&ctx->db, &key, &key_len))
                {
                    void  *val;
                    size_t val_len;
                    char   key_copy[KEY_LEN];
                    size_t copy_len = (key_len < sizeof(key_copy) - 1) ? key_len : sizeof(key_copy) - 1;

                    p101_memcpy(env, key_copy, key, copy_len);
                    key_copy[copy_len] = '\0';

                    if(db_fetch(&ctx->db, key_copy, copy_len, &val, &val_len) == 1)
                    {
                        printf("%.*s: %.*s\n", (int)copy_len, key_copy, (int)val_len, (char *)val);
                    }
                }
                if(r < 0)
                {
                    P101_ERROR_RAISE_USER(err, "Iteration failed", ERR_SYSTEM);
                }
                break;
            }
            case 'd': /* delete */
            {
                printf("Key: ");
                if(scanf("%1023s", key_buf) == 1)
                {
                    if(db_delete(&ctx->db, key_buf, strlen(key_buf)) != 0)
                    {
                        printf("Delete failed (key may not exist).\n");
                    }
                }
                break;
            }
            default:
            {
                printf("Commands: s(tore), f(etch), l(ist), d(elete), q(uit)\n");
                break;
            }
        }

        /* If a business logic error occurred, we might want to break the loop or handle it */
        if(p101_error_has_error(err))
        {
            fprintf(stderr, "Runtime error: %s\n", p101_error_get_message(err));
            p101_error_reset(err);
        }
    }

    return CLEANUP;
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

    fprintf(stderr, "Usage: %s -d <database_path> [-h] [-s <semaphore>] \n", ctx->arguments->program_name);
    fputs("Options:\n", stderr);
    fputs("  -h                Display this help message and exit\n", stderr);
    fputs("  -d <database_path>     Directory to give to clients on connection (default HOME)\n", stderr);
    fputs("  -s <semaphore>         Port to host on (default 8000)\n", stderr);

    return CLEANUP;
}

static p101_fsm_state_t cleanup(const struct p101_env *env, struct p101_error *err, void *context)
{
    struct context *ctx;

    P101_TRACE(env);
    ctx = (struct context *)context;

    db_close(&ctx->db);

    if(p101_error_has_error(err))
    {
        p101_error_default_error_reporter(err);
        p101_error_reset(err);
        ctx->exit_code = EXIT_FAILURE;
    }

    return P101_FSM_EXIT;
}