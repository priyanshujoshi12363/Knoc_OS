#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ulib.h"

#define REDIRECTS_MAX 3
#define HEADER_MAX 4096

typedef struct url
{
    char host[128];
    unsigned int port;
    char path[512];
} url_t;

static char header[HEADER_MAX];
static char buffer[4096];

static int parse_url(const char *text, url_t *url)
{
    if (strncmp(text, "https://", 8) == 0)
    {
        return -2;
    }

    if (strncmp(text, "http://", 7) == 0)
    {
        text += 7;
    }

    const char *slash = strchr(text, '/');
    size_t host_length = slash ? (size_t)(slash - text) : strlen(text);
    const char *colon = memchr(text, ':', host_length);

    url->port = 80;

    if (colon)
    {
        url->port = (unsigned int)atoi(colon + 1);
        host_length = (size_t)(colon - text);
    }

    if (host_length == 0 || host_length >= sizeof(url->host) || url->port == 0)
    {
        return -1;
    }

    memcpy(url->host, text, host_length);
    url->host[host_length] = 0;
    snprintf(url->path, sizeof(url->path), "%s", slash ? slash : "/");
    return 0;
}

static const char *default_name(const url_t *url)
{
    const char *base = strrchr(url->path, '/');

    base = base ? base + 1 : url->path;
    return *base ? base : "index.html";
}

static int header_value(const char *name, char *out, size_t room)
{
    size_t length = strlen(name);

    for (char *line = strstr(header, "\r\n"); line; line = strstr(line + 2, "\r\n"))
    {
        if (strncasecmp(line + 2, name, length) == 0 && line[2 + length] == ':')
        {
            const char *value = line + 3 + length;
            size_t n = 0;

            while (*value == ' ')
            {
                value++;
            }

            while (value[n] && value[n] != '\r' && n < room - 1)
            {
                out[n] = value[n];
                n++;
            }

            out[n] = 0;
            return 1;
        }
    }

    return 0;
}

int main(int argc, char **argv)
{
    url_t url;
    char location[512];
    const char *target = argc > 1 ? argv[1] : 0;

    if (!target)
    {
        printf("usage: fetch URL [FILE]   (http:// only)\n");
        return 1;
    }

    for (int redirect = 0; redirect <= REDIRECTS_MAX; redirect++)
    {
        int parsed = parse_url(target, &url);

        if (parsed == -2)
        {
            printf("fetch: https needs encryption (TLS), which KnocOS doesn't have yet; use http://\n");
            return 1;
        }

        if (parsed != 0)
        {
            printf("fetch: bad URL %s\n", target);
            return 1;
        }

        unsigned int ip;
        int result = net_resolve(url.host, &ip);

        if (result != 0)
        {
            printf("fetch: cannot find %s\n", url.host);
            return 1;
        }

        int connection = tcp_connect(ip, url.port);

        if (connection < 0)
        {
            printf("fetch: cannot connect to %s:%u (%s)\n", url.host, url.port,
                   connection == E_REFUSED ? "refused" : connection == E_TIMEOUT ? "timed out" : "error");
            return 1;
        }

        int request_length = snprintf(buffer, sizeof(buffer),
                                      "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: KnocOS-fetch/0.27\r\n"
                                      "Connection: close\r\n\r\n",
                                      url.path, url.host);

        if (tcp_send(connection, buffer, (unsigned long)request_length) != request_length)
        {
            printf("fetch: sending the request failed\n");
            tcp_close(connection);
            return 1;
        }

        size_t header_length = 0;
        char *body = 0;
        long got = 0;

        while (!body && header_length < HEADER_MAX - 1 &&
               (got = tcp_recv(connection, header + header_length, HEADER_MAX - 1 - header_length)) > 0)
        {
            header_length += (size_t)got;
            header[header_length] = 0;
            body = strstr(header, "\r\n\r\n");
        }

        if (!body)
        {
            printf("fetch: no HTTP answer from %s\n", url.host);
            tcp_close(connection);
            return 1;
        }

        *body = 0;
        body += 4;

        int status = 0;

        sscanf(header, "HTTP/%*s %d", &status);

        if (status == 0)
        {
            char version[16];

            sscanf(header, "%15s %d", version, &status);
        }

        if ((status == 301 || status == 302 || status == 307 || status == 308) &&
            header_value("Location", location, sizeof(location)))
        {
            tcp_close(connection);
            printf("fetch: %d, following %s\n", status, location);
            target = location;
            continue;
        }

        if (status != 200)
        {
            printf("fetch: the server answered %d\n", status);
            tcp_close(connection);
            return 1;
        }

        const char *name = argc > 2 ? argv[2] : default_name(&url);
        FILE *out = fopen(name, "w");

        if (!out)
        {
            printf("fetch: cannot write %s\n", name);
            tcp_close(connection);
            return 1;
        }

        unsigned long started = uptime();
        size_t total = header_length - (size_t)(body - header);

        fwrite(body, 1, total, out);

        while ((got = tcp_recv(connection, buffer, sizeof(buffer))) > 0)
        {
            fwrite(buffer, 1, (size_t)got, out);
            total += (size_t)got;
        }

        fclose(out);
        tcp_close(connection);

        if (got < 0)
        {
            printf("fetch: the connection broke after %zu bytes\n", total);
            return 1;
        }

        unsigned long ticks = uptime() - started;

        printf("fetch: 200 OK, %zu bytes saved to %s (%lu.%02lu s)\n", total, name, ticks / 100, ticks % 100);
        return 0;
    }

    printf("fetch: too many redirects\n");
    return 1;
}
