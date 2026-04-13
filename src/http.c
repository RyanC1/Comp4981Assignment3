#include "http.h"
#include "db_util.h"
#include "util.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define REQUEST_TIMEOUT_SEC 10    // NOLINT(cppcoreguidelines-macro-to-enum,modernize-macro-to-enum)
#define SEARCH_RANGE 128          // NOLINT(cppcoreguidelines-macro-to-enum,modernize-macro-to-enum)
#define INT_BASE 10               // NOLINT(cppcoreguidelines-macro-to-enum,modernize-macro-to-enum)
#define VERSION 1                 // NOLINT(cppcoreguidelines-macro-to-enum,modernize-macro-to-enum)

/* Maximum byte length accepted for a POST key (the request path). */
#define MAX_POST_KEY_LEN 512    // NOLINT(cppcoreguidelines-macro-to-enum,modernize-macro-to-enum)

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

/*
 * str_iequal_n: case-insensitive comparison of first n bytes.
 * Replaces strncasecmp (POSIX extension) per coding requirements.
 */
static bool str_iequal_n(const char *a, const char *b, size_t n)
{
    size_t i;

    for(i = 0; i < n; i++)
    {
        if(tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
        {
            return false;
        }
    }
    return true;
}

/*
 * get_header_val: locate a header value within the raw header block.
 *
 * Searches for "\r\nKey:" to avoid false matches inside values.
 * Returns a pointer to the first non-space byte of the value field.
 * The value is NOT null-terminated; it ends at the next \r\n.
 * Returns NULL when the header is absent.
 */
static const char *get_header_val(const char *headers, const char *key)
{
    char        search[SEARCH_RANGE];
    const char *ptr;

    snprintf(search, sizeof(search), "\r\n%s:", key);
    ptr = strstr(headers, search);
    if(!ptr)
    {
        return NULL;
    }
    ptr += strlen(search);
    while(*ptr == ' ')
    {
        ptr++;
    }
    return ptr;
}

int get_version(void)
{
    return VERSION;
}

/* ------------------------------------------------------------------ */
/* http_read_request                                                    */
/*                                                                      */
/* Sets SO_RCVTIMEO + SO_SNDTIMEO (10 s) so that every subsequent I/O */
/* on this fd — including http_send_response — times out automatically.*/
/*                                                                      */
/* Returns  0  success (method may still be METHOD_UNKNOWN/INVALID;    */
/*              http_validate_request will reject those with 400/501)  */
/*         -1  hard I/O error, timeout, or allocation failure          */
/* ------------------------------------------------------------------ */

int http_read_request(int client_sock, HttpRequest *out_req)
{
    struct timeval tv;
    char          *buf;
    char          *boundary;
    const char    *ka_val;
    const char    *cl_val;
    const char    *body_start;
    char           m_str[OTHER_HEADER_FIELDS_MAX];
    char           p_str[MAX_HEADER_PATH_SIZE];
    char           v_str[OTHER_HEADER_FIELDS_MAX];
    ssize_t        header_bytes;
    size_t         header_len;

    memset(out_req, 0, sizeof(HttpRequest));

    /* Apply timeout to both recv and send so http_send_response is also
     * covered without a separate setsockopt call there. */
    tv.tv_sec  = REQUEST_TIMEOUT_SEC;
    tv.tv_usec = 0;
    if(setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0 || setsockopt(client_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0)
    {
        return -1;
    }

    buf = (char *)malloc(MAX_HEADER_SIZE);
    if(!buf)
    {
        return -1;
    }

    /* Read one byte at a time until \r\n\r\n is found or the buffer fills. */
    header_bytes = safe_read_delimited(client_sock, buf, MAX_HEADER_SIZE - 1, "\r\n\r\n");
    if(header_bytes < 0)
    {
        free(buf);
        return -1;
    }
    buf[header_bytes] = '\0';

    boundary = strstr(buf, "\r\n\r\n");
    if(!boundary)
    {
        /* Buffer filled without a complete header block. */
        free(buf);
        return -1;
    }

    body_start = boundary + 4;
    header_len = (size_t)(body_start - buf);

    out_req->raw_headers = (char *)malloc(header_len + 1);
    if(!out_req->raw_headers)
    {
        free(buf);
        return -1;
    }
    memcpy(out_req->raw_headers, buf, header_len);
    out_req->raw_headers[header_len] = '\0';

    /* Parse the request line.  Leave method/version zero-initialised on
     * failure; http_validate_request will respond with 400. */
    if(sscanf(buf, "%15s %1023s %15s", m_str, p_str, v_str) != 3)
    {
        free(buf);
        return 0;
    }

    if(strcmp(m_str, "GET") == 0)
    {
        out_req->method = METHOD_GET;
    }
    else if(strcmp(m_str, "POST") == 0)
    {
        out_req->method = METHOD_POST;
    }
    else if(strcmp(m_str, "HEAD") == 0)
    {
        out_req->method = METHOD_HEAD;
    }
    else
    {
        out_req->method = METHOD_INVALID;
    }

    if(strcmp(v_str, "HTTP/1.0") == 0 || strcmp(v_str, "HTTP/1.1") == 0)
    {
        out_req->version = VERSION_1_0;
    }
    else
    {
        out_req->version = VERSION_UNKNOWN;
    }

    strncpy(out_req->path, p_str, sizeof(out_req->path) - 1);
    out_req->path[sizeof(out_req->path) - 1] = '\0';

    /* Keep-Alive (case-insensitive, no strncasecmp) */
    ka_val = get_header_val(out_req->raw_headers, "Connection");
    if(ka_val && str_iequal_n(ka_val, "keep-alive", strlen("keep-alive")))
    {
        out_req->keep_alive = true;
    }

    /* Body: read exactly Content-Length bytes */
    cl_val = get_header_val(out_req->raw_headers, "Content-Length");
    if(cl_val)
    {
        out_req->body_len = (size_t)strtoul(cl_val, NULL, INT_BASE);
    }

    if(out_req->body_len > 0)
    {
        ssize_t remaining;
        ssize_t already_in_buf;

        if(out_req->body_len > (size_t)MAX_BODY_SIZE)
        {
            free(out_req->raw_headers);
            out_req->raw_headers = NULL;
            free(buf);
            return -1;
        }

        out_req->body = (char *)malloc(out_req->body_len + 1);
        if(!out_req->body)
        {
            free(out_req->raw_headers);
            out_req->raw_headers = NULL;
            free(buf);
            return -1;
        }

        /* safe_read_delimited stops exactly at the delimiter, so
         * already_in_buf is normally 0; the guard is kept for safety. */
        already_in_buf = header_bytes - (ssize_t)header_len;
        if(already_in_buf > (ssize_t)out_req->body_len)
        {
            already_in_buf = (ssize_t)out_req->body_len;
        }
        if(already_in_buf > 0)
        {
            memcpy(out_req->body, body_start, (size_t)already_in_buf);
        }

        remaining = (ssize_t)out_req->body_len - already_in_buf;
        while(remaining > 0)
        {
            ssize_t n;
            size_t  offset = out_req->body_len - (size_t)remaining;

            n = read(client_sock, out_req->body + offset, (size_t)remaining);
            if(n <= 0)
            {
                free(out_req->body);
                out_req->body = NULL;
                free(out_req->raw_headers);
                out_req->raw_headers = NULL;
                free(buf);
                return -1;
            }
            remaining -= n;
        }
        out_req->body[out_req->body_len] = '\0';
    }

    free(buf);
    return 0;
}

/* ------------------------------------------------------------------ */
/* http_validate_request                                                */
/*                                                                      */
/* Returns false and populates res with 400 or 501 on rejection.       */
/* ------------------------------------------------------------------ */

bool http_validate_request(const HttpRequest *req, HttpResponse *res)
{
    memset(res, 0, sizeof(HttpResponse));
    res->file_fd = -1;

    if(req->version != VERSION_1_0)
    {
        res->status_code    = BAD_REQUEST;
        res->status_message = "Bad Request";
        return false;
    }
    if(req->method == METHOD_UNKNOWN)
    {
        res->status_code    = NOT_IMPLEMENTED;
        res->status_message = "Not Implemented";
        return false;
    }
    if(req->method == METHOD_INVALID)
    {
        res->status_code    = BAD_REQUEST;
        res->status_message = "Bad Request Method";
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* post_validate_request                                                */
/*                                                                      */
/* Checks all POST-specific preconditions before touching the database. */
/* Populates res with the appropriate 4xx code on failure.             */
/* Returns true when the request is safe to proceed.                   */
/* ------------------------------------------------------------------ */

static bool post_validate_request(const HttpRequest *req, HttpResponse *res)
{
    size_t path_len;

    /* Body must be present and non-empty. */
    if(!req->body || req->body_len == 0)
    {
        res->status_code    = BAD_REQUEST;
        res->status_message = "Bad Request: empty body";
        return false;
    }

    /* Path must be present and non-empty (it becomes the DB key). */
    path_len = strlen(req->path);
    if(path_len == 0)
    {
        res->status_code    = BAD_REQUEST;
        res->status_message = "Bad Request: missing path";
        return false;
    }

    /* Guard against unreasonably long keys to avoid ndbm block overflow. */
    if(path_len > MAX_POST_KEY_LEN)
    {
        res->status_code    = BAD_REQUEST;
        res->status_message = "Bad Request: path too long";
        return false;
    }

    return true;
}

/* ------------------------------------------------------------------ */
/* http_handle_operation                                                */
/*                                                                      */
/* Returns  0  res fully populated (may carry an HTTP error status)    */
/*         -1  hard system error (fstat failure, malloc); res invalid  */
/* ------------------------------------------------------------------ */

int http_handle_operation(HttpRequest *req, HttpResponse *res, sem_t *sem)
{
    struct stat st;

    res->keep_alive = req->keep_alive;
    res->file_fd    = -1;
    res->file_size  = 0;

    if(req->method == METHOD_GET || req->method == METHOD_HEAD)
    {
        /* ----------------------------------------------------------
         * Serve a file from the current working directory.
         * ---------------------------------------------------------- */
        int         fd;
        const char *file_path;

        if((req->path[0] == '/' && req->path[1] == '\0') || req->path[0] == '\0')
        {
            file_path = "index.html";
        }
        else
        {
            file_path = (req->path[0] == '/') ? req->path + 1 : req->path;
        }

        fd = open(file_path, O_RDONLY | O_CLOEXEC);
        if(fd < 0)
        {
            res->status_code    = NOT_FOUND;
            res->status_message = "Not Found";
            return 0;
        }

        if(fstat(fd, &st) < 0)
        {
            close(fd);
            return -1; /* hard kernel error */
        }

        res->status_code    = OK;
        res->status_message = "OK";
        res->file_size      = (size_t)st.st_size;

        if(req->method == METHOD_GET)
        {
            res->file_fd = fd; /* caller closes via http_send_response */
        }
        else
        {
            /* HEAD: report size but send no body */
            close(fd);
            res->file_fd = -1;
        }
    }
    else if(req->method == METHOD_POST)
    {
        /* ----------------------------------------------------------
         * Write the request body to the shared ndbm database, keyed
         * by the request path.
         *
         * Locking order:
         *   1. validate inputs (no lock needed — read-only)
         *   2. sem_wait        (blocks concurrent writers)
         *   3. db_open
         *   4. db_store
         *   5. db_close
         *   6. sem_post
         *
         * The semaphore is held for the smallest possible window.
         * ---------------------------------------------------------- */
        DbHandle db;
        int      rc;

        if(!post_validate_request(req, res))
        {
            return 0; /* res already populated with 4xx */
        }

        if(sem_wait(sem) != 0)
        {
            /* Interrupted by a signal or other error — do not proceed. */
            res->status_code    = SERVER_ERROR;
            res->status_message = "Internal Server Error";
            return 0;
        }

        memset(&db, 0, sizeof(DbHandle));
        if(db_open(&db) != 0)
        {
            sem_post(sem);
            res->status_code    = SERVER_ERROR;
            res->status_message = "Internal Server Error";
            return 0;
        }

        rc = db_store(&db, req->path, strlen(req->path), req->body, req->body_len);

        db_close(&db);
        sem_post(sem);

        if(rc != 0)
        {
            res->status_code    = SERVER_ERROR;
            res->status_message = "Internal Server Error";
            return 0;
        }

        res->status_code    = CREATED;
        res->status_message = "Created";
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* http_send_response                                                   */
/*                                                                      */
/* SO_SNDTIMEO set in http_read_request covers these writes too.       */
/* Returns 0 on success, -1 on write failure or timeout.               */
/* ------------------------------------------------------------------ */

int http_send_response(int client_sock, HttpResponse *res)
{
    char head[MAX_HEADER_SIZE];
    int  head_len;

    head_len = snprintf(head,
                        sizeof(head),
                        "HTTP/1.0 %d %s\r\n"
                        "Content-Length: %lu\r\n"
                        "Connection: %s\r\n"
                        "\r\n",
                        res->status_code,
                        res->status_message,
                        (unsigned long)res->file_size,
                        res->keep_alive ? "keep-alive" : "close");

    if(head_len < 0 || (size_t)head_len >= sizeof(head))
    {
        if(res->file_fd != -1)
        {
            close(res->file_fd);
            res->file_fd = -1;
        }
        return -1;
    }

    if(safe_write(client_sock, head, (size_t)head_len) < 0)
    {
        if(res->file_fd != -1)
        {
            close(res->file_fd);
            res->file_fd = -1;
        }
        return -1;
    }

    if(res->file_fd != -1)
    {
        if(copy(res->file_fd, client_sock) < 0)
        {
            close(res->file_fd);
            res->file_fd = -1;
            return -1;
        }
        close(res->file_fd);
        res->file_fd = -1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* http_free_request                                                    */
/* ------------------------------------------------------------------ */

void http_free_request(HttpRequest *req)
{
    if(req->raw_headers)
    {
        free(req->raw_headers);
        req->raw_headers = NULL;
    }
    if(req->body)
    {
        free(req->body);
        req->body = NULL;
    }
}