#ifndef WATCHER_H
#define WATCHER_H

#include <p101_fsm/fsm.h>
#include <stdint.h>
#include <stdnoreturn.h>
#include <sys/types.h>



struct wchr_context {
    uint8_t        children;
    const char *semaphore;
    int parent_fd;
    char *lib_path;

    int exit_code;
};

// Entry point
noreturn void run_watcher_fsm(const struct p101_env *env, struct p101_error *err, uint8_t children, const char*semaphore, int parent_fd, char *lib_path);

#endif // WATCHER_H
