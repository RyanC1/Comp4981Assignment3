#ifndef UTIL_H
#define UTIL_H

#include <p101_env/env.h>
#include <sys/types.h>

ssize_t safe_read_delimited(int fd, void *buf, size_t count, const char *delim);

ssize_t safe_write(int fd, const void *buf, size_t n);

ssize_t copy(int source, int destination);

ssize_t copy_delimited(int source, int destination, const char *delim);

char *concat_string(const char *str1, const char *str2);

#endif    // UTIL_H
