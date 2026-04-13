#ifndef CONTEXT_H
#define CONTEXT_H

#include <netinet/in.h>
#include <sys/socket.h>

struct context
{
    struct arguments *arguments;

    in_port_t port;
    struct sockaddr_storage addr;
    uint8_t children;
    const char *semaphore;


    int client_socket_fd; //outgoing socket
    int client_con_fd; //connected client

    pid_t watcher_pid;
    int worker_socket_fd;
    char *lib_path;

    int exit_code;
};

#endif    // CONTEXT_H
