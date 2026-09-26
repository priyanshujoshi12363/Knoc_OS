#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "http.h"
#include "ulib.h"

typedef struct download
{
    const char *name;
    char default_name[FILE_NAME_MAX];
    FILE *out;
} download_t;

static void pick_name(download_t *d, const char *url)
{
    http_url_t parsed;
    const char *base = "index.html";

    if (http_parse_url(url, &parsed) == 0)
    {
        parsed.path[strcspn(parsed.path, "?")] = 0;

        const char *slash = strrchr(parsed.path, '/');

        if (slash && slash[1])
        {
            base = slash + 1;
        }
    }

    snprintf(d->default_name, sizeof(d->default_name), "%s", base);
}

static int save(void *context, const char *data, size_t length, http_response_t *response)
{
    download_t *d = context;

    if (response->status != 200)
    {
        return 0;
    }

    if (!d->out)
    {
        if (!d->name)
        {
            pick_name(d, response->url);
            d->name = d->default_name;
        }

        d->out = fopen(d->name, "w");

        if (!d->out)
        {
            snprintf(response->error, sizeof(response->error), "cannot write %s", d->name);
            return 1;
        }
    }

    if (fwrite(data, 1, length, d->out) != length)
    {
        snprintf(response->error, sizeof(response->error), "writing %s failed (disk full?)", d->name);
        return 1;
    }

    return 0;
}

int main(int argc, char **argv)
{
    download_t d;
    http_response_t response;

    if (argc < 2)
    {
        printf("usage: fetch URL [FILE]   (http:// and https://)\n");
        return 1;
    }

    memset(&d, 0, sizeof(d));
    d.name = argc > 2 ? argv[2] : 0;

    unsigned long started = uptime();
    int result = http_get(argv[1], save, &d, &response);

    if (d.out)
    {
        fclose(d.out);
    }

    if (result != 0)
    {
        if (d.out)
        {
            remove(d.name);
        }

        printf("fetch: %s\n", response.error);
        return 1;
    }

    if (response.redirects > 0)
    {
        printf("fetch: redirected to %s\n", response.url);
    }

    if (response.status != 200)
    {
        printf("fetch: the server answered %d\n", response.status);
        return 1;
    }

    if (!d.out)
    {
        save(&d, "", 0, &response);

        if (d.out)
        {
            fclose(d.out);
        }
    }

    unsigned long ticks = uptime() - started;

    printf("fetch: 200 OK, %ld bytes saved to %s (%lu.%02lu s)\n", response.received, d.name, ticks / 100,
           ticks % 100);
    return 0;
}
