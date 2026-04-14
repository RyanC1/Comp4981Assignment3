#include "socket_util.h"
#include <p101_c/p101_string.h>
#include <p101_posix/sys/p101_socket.h>
#include <stdio.h>

int convert_address(const struct p101_env *env, struct p101_error *err, const char *address, struct sockaddr_storage *addr)
{
    p101_memset(env, addr, 0, sizeof(*addr));

    if(inet_pton(AF_INET, address, &(((struct sockaddr_in *)addr)->sin_addr)) == 1)
    {
        addr->ss_family = AF_INET;
    }
    else if(inet_pton(AF_INET6, address, &(((struct sockaddr_in6 *)addr)->sin6_addr)) == 1)
    {
        addr->ss_family = AF_INET6;
    }
    else
    {
        P101_ERROR_RAISE_ERRNO(err, errno);
        return -1;
    }

    return 0;
}

int create_socket(const struct p101_env *env, struct p101_error *err, const int family)
{
    int socket_fd;

    socket_fd = p101_socket(env, err, family, SOCK_STREAM, 0);    // NOLINT(android-cloexec-socket)

    return socket_fd;
}

int socket_bind(const struct p101_env *env, struct p101_error *err, int sockfd, struct sockaddr_storage *addr, in_port_t port)
{
    socklen_t addr_len;
    in_port_t net_port;
    net_port = htons(port);

    P101_TRACE(env);

    if(addr->ss_family == AF_INET)
    {
        struct sockaddr_in *ipv4_addr;

        ipv4_addr           = (struct sockaddr_in *)addr;
        addr_len            = sizeof(*ipv4_addr);
        ipv4_addr->sin_port = net_port;
    }
    else if(addr->ss_family == AF_INET6)
    {
        struct sockaddr_in6 *ipv6_addr;

        ipv6_addr            = (struct sockaddr_in6 *)addr;
        addr_len             = sizeof(*ipv6_addr);
        ipv6_addr->sin6_port = net_port;
    }
    else
    {
        P101_ERROR_RAISE_ERRNO(err, EAFNOSUPPORT);
        return -1;
    }

    p101_bind(env, err, sockfd, (struct sockaddr *)addr, addr_len);

    if(p101_error_has_error(err))
    {
        return -1;
    }
    return 0;
}

int socket_connect(const struct p101_env *env, struct p101_error *err, int sockfd, struct sockaddr_storage *addr, in_port_t port)
{
    socklen_t addr_len;
    in_port_t net_port;
    net_port = htons(port);

    P101_TRACE(env);

    if(addr->ss_family == AF_INET)
    {
        struct sockaddr_in *ipv4_addr;

        ipv4_addr           = (struct sockaddr_in *)addr;
        addr_len            = sizeof(*ipv4_addr);
        ipv4_addr->sin_port = net_port;
    }
    else if(addr->ss_family == AF_INET6)
    {
        struct sockaddr_in6 *ipv6_addr;

        ipv6_addr            = (struct sockaddr_in6 *)addr;
        addr_len             = sizeof(*ipv6_addr);
        ipv6_addr->sin6_port = net_port;
    }
    else
    {
        P101_ERROR_RAISE_ERRNO(err, EAFNOSUPPORT);
        return -1;
    }

    p101_connect(env, err, sockfd, (struct sockaddr *)addr, addr_len);

    if(p101_error_has_error(err))
    {
        return -1;
    }
    return 0;
}

int print_socket(const struct p101_env *env, struct p101_error *err, int sockfd)
{
    struct sockaddr_in addr;
    socklen_t          addrlen;
    char               ip_str[INET_ADDRSTRLEN];

    P101_TRACE(env);

    addrlen = sizeof(addr);

    p101_getsockname(env, err, sockfd, (struct sockaddr *)&addr, &addrlen);

    if(p101_error_has_error(err))
    {
        return -1;
    }

    printf("%s:%d\n", inet_ntop(AF_INET, &(addr.sin_addr), ip_str, sizeof(ip_str)), ntohs(addr.sin_port));
    fflush(stdout);

    return 0;
}
