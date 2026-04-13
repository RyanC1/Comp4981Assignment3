#include "util.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define COPY_BUFF 10    // NOLINT(cppcoreguidelines-macro-to-enum, modernize-macro-to-enum)

ssize_t safe_read_delimited(int fd, void *buf, size_t count, const char *delim)
{
    uint8_t *buf_pos;
    size_t   total;
    size_t   run;
    ssize_t  n;

    buf_pos = buf;
    total   = 0;
    run     = 0;

    do
    {
        n = read(fd, buf_pos + total, 1);
        if(n > 0)
        {
            if(buf_pos[total] == (uint8_t)delim[run])
            {
                run++;
            }
            else
            {
                run = 0;
            }
            total++;
        }
        else if(n == -1)
        {
            return -1;
        }

    } while(total < count && n != 0 && delim[run] != '\0');

    return (ssize_t)total;
}

ssize_t safe_write(int fd, const void *buf, size_t n)
{
    const uint8_t *p;
    size_t         left;

    p    = (const uint8_t *)buf;
    left = n;
    while(left > 0)
    {
        ssize_t w;
        w = write(fd, p, left);
        if(w > 0)
        {
            p += (size_t)w;
            left -= (size_t)w;
            continue;
        }

        return -1;
    }
    return (ssize_t)n;
}

ssize_t copy(int source, int destination)
{
    ssize_t n;
    ssize_t total;
    char    c[COPY_BUFF];

    total = 0;

    do
    {
        n = read(source, c, COPY_BUFF);
        if(n == -1)
        {
            return -1;
        }

        if(n > 0)
        {
            if(safe_write(destination, c, (size_t)n) == -1)
            {
                return -1;
            }
            total += n;
        }
    } while(n > 0);

    return total;
}

ssize_t copy_delimited(int source, int destination, const char *delim)
{
    ssize_t n;
    ssize_t total;
    char    c[1];
    size_t  run;

    total = 0;
    run   = 0;

    do
    {
        n = read(source, c, 1);
        if(n == -1)
        {
            return -1;
        }

        if(n > 0)
        {
            if(safe_write(destination, c, 1) == -1)
            {
                return -1;
            }
            if(c[0] == (int8_t)delim[run])
            {
                run++;
            }
            else
            {
                run = 0;
            }

            total += n;
        }
    } while(n > 0 && delim[run] != '\0');

    return total;
}

char *concat_string(const char *str1, const char *str2)
{
    size_t len = strlen(str1) + strlen(str2);

    char *result = (char *)malloc(len + 1);
    if(result == NULL)
    {
        return NULL;
    }
    memset(result, 0, len);

    sprintf(result, "%s%s", str1, str2);

    return result;
}
