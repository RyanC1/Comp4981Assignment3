#ifndef HTTP_H
#define HTTP_H

#include <semaphore.h>
#include <stdbool.h>
#include <stddef.h>


#define MAX_HEADER_SIZE 8192 // NOLINT(cppcoreguidelines-macro-to-enum, modernize-macro-to-enum)
#define MAX_HEADER_PATH_SIZE 1024 // NOLINT(cppcoreguidelines-macro-to-enum, modernize-macro-to-enum)
#define MAX_BODY_SIZE (1024 * 1024 * 16)    // NOLINT(cppcoreguidelines-macro-to-enum, modernize-macro-to-enum)
#define OTHER_HEADER_FIELDS_MAX 16 // NOLINT(cppcoreguidelines-macro-to-enum, modernize-macro-to-enum)

enum
{
    INFORMATIONAL = 100,
    OK = 200,
    CREATED = 201,
    ACCEPTED = 202,
    NO_CONTENT = 204,
    MULTIPLE_CHOICES = 300,
    MOVED_PERMANENTLY = 301,
    MOVED_TEMPORARILY = 302,
    NOT_MODIFIED = 304,
    BAD_REQUEST = 400,
    UNAUTHORIZED = 401,
    FORBIDDEN = 403,
    NOT_FOUND = 404,
    SERVER_ERROR = 500,
    NOT_IMPLEMENTED = 501,
    BAD_GATEWAY = 502,
    SERVICE_UNAVAILABLE = 503
};

typedef enum {
    METHOD_UNKNOWN,
    METHOD_GET,
    METHOD_POST,
    METHOD_HEAD,
    METHOD_INVALID
} HttpMethod;

typedef enum {
    VERSION_UNKNOWN,
    VERSION_1_0,
    VERSION_INVALID
} HttpVersion;

typedef struct {
    HttpMethod  method;
    char        path[MAX_HEADER_PATH_SIZE];
    HttpVersion version;
    char       *raw_headers;
    char       *body;
    size_t      body_len;
    bool        keep_alive;
} HttpRequest;

typedef struct {
    int         status_code;
    const char *status_message;
    int         file_fd;
    size_t      file_size;
    bool        keep_alive;
} HttpResponse;

int  http_read_request    (int client_sock, HttpRequest  *out_req);
bool http_validate_request(const HttpRequest *req, HttpResponse *res);
int  http_handle_operation(HttpRequest *req, HttpResponse *res, sem_t *sem);
int  http_send_response   (int client_sock, HttpResponse *res);
void http_free_request    (HttpRequest *req);
int get_version(void);

#endif
