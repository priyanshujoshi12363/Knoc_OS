#ifndef KNOC_HTTP_H
#define KNOC_HTTP_H

#include <stddef.h>

#define HTTP_URL_MAX 1024
#define HTTP_HOST_MAX 128

typedef struct http_url
{
    int secure;
    char host[HTTP_HOST_MAX];
    unsigned int port;
    char path[HTTP_URL_MAX];
} http_url_t;

typedef struct http_response
{
    int status;
    int redirects;
    char url[HTTP_URL_MAX];
    char content_type[128];
    long content_length;
    long received;
    char error[200];
} http_response_t;

typedef int (*http_sink_t)(void *context, const char *data, size_t length, http_response_t *response);

int http_parse_url(const char *text, http_url_t *url);
int http_join(const char *base, const char *link, char *out, size_t room);
int http_get(const char *url, http_sink_t sink, void *context, http_response_t *response);

#endif
