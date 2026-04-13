#ifndef ARGUMENTS_H
#define ARGUMENTS_H

struct arguments
{
    int argc;
    const char *program_name;

    const char *raw_ip;
    const char *raw_port;
    const char *raw_source;
    const char *raw_children;
    const char *raw_semaphore;

    char **argv;
};

#endif    // ARGUMENTS_H
