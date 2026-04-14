#ifndef SIGNAL_UTIL_H
#define SIGNAL_UTIL_H
#include <signal.h>

extern volatile sig_atomic_t exit_flag;    // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

void setup_signal_handlers(void);

void sig_handler(int signal);

#endif //SIGNAL_UTIL_H
