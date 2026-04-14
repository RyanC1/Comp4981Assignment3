#include "signal_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

volatile sig_atomic_t exit_flag = 0;    // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

void setup_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(struct sigaction));

#ifdef __clang__
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wdisabled-macro-expansion"
#endif
    action.sa_handler = sig_handler;
#ifdef __clang__
    #pragma clang diagnostic pop
#endif

    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;

    if(sigaction(SIGINT, &action, NULL) == -1)
    {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }

    if(sigaction(SIGPIPE, &action, NULL) == -1)
    {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
}

void sig_handler(int signal)
{
    if(signal == SIGINT)
    {
        exit_flag = 1;
    }
}
