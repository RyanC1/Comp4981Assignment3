#ifndef SOCKET_UTIL_H
#define SOCKET_UTIL_H

#include <p101_env/env.h>
#include <p101_posix/arpa/p101_inet.h>



int convert_address(const struct p101_env *env, struct p101_error *err, const char *address, struct sockaddr_storage *addr);

int create_socket(const struct p101_env *env, struct p101_error *err, int family);

int socket_bind(const struct p101_env *env, struct p101_error *err, int sockfd, struct sockaddr_storage *addr, in_port_t port);

int socket_connect(const struct p101_env *env, struct p101_error *err, int sockfd, struct sockaddr_storage *addr, in_port_t port);

int print_socket(const struct p101_env *env, struct p101_error *err, int sockfd);

#endif    // SOCKET_UTIL_H
