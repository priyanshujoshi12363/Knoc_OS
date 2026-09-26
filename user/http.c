#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "bearssl.h"
#include "http.h"
#include "ulib.h"

#define REDIRECTS_MAX 5
#define HEADER_MAX 16384
#define CERTS_FILE "/etc/ssl/certs.pem"
#define CERTS_MAX (1024 * 1024)
#define ANCHORS_MAX 400
#define UNIX_TO_DAY_ZERO 719528L
#define SEED_BYTES 32
#define TLS_ALERT_PROTOCOL_VERSION 70

typedef struct bytes
{
    unsigned char *data;
    size_t length;
    size_t room;
} bytes_t;

typedef struct connection
{
    int tcp;
    int secure;
    int tcp_closed;
} connection_t;

enum
{
    CHUNK_SIZE,
    CHUNK_EXTENSION,
    CHUNK_DATA,
    CHUNK_DATA_END,
    CHUNK_DONE
};

typedef struct body
{
    http_sink_t sink;
    void *context;
    http_response_t *response;
    int chunked;
    int chunk_state;
    long chunk_left;
    int aborted;
} body_t;

static char header[HEADER_MAX + 1];
static unsigned char buffer[8192];
static br_ssl_client_context client;
static br_x509_minimal_context validator;
static unsigned char io_buffer[BR_SSL_BUFSIZE_BIDI];
static br_sslio_context io;
static br_x509_trust_anchor *anchors;
static size_t anchor_count;
static int anchors_loaded;

static int append_bytes(bytes_t *b, const void *data, size_t length)
{
    if (b->length + length > b->room)
    {
        size_t room = b->room ? b->room * 2 : 256;

        while (room < b->length + length)
        {
            room *= 2;
        }

        unsigned char *grown = realloc(b->data, room);

        if (!grown)
        {
            return -1;
        }

        b->data = grown;
        b->room = room;
    }

    memcpy(b->data + b->length, data, length);
    b->length += length;
    return 0;
}

static void append_callback(void *context, const void *data, size_t length)
{
    append_bytes(context, data, length);
}

static unsigned char *copy_of(const unsigned char *data, size_t length)
{
    unsigned char *copy = malloc(length ? length : 1);

    if (copy)
    {
        memcpy(copy, data, length);
    }

    return copy;
}

static void add_anchor(const unsigned char *der, size_t length)
{
    br_x509_decoder_context decoder;
    bytes_t name = {0, 0, 0};

    if (anchor_count >= ANCHORS_MAX)
    {
        return;
    }

    br_x509_decoder_init(&decoder, append_callback, &name);
    br_x509_decoder_push(&decoder, der, length);

    br_x509_pkey *key = br_x509_decoder_get_pkey(&decoder);

    if (!key || !name.data)
    {
        free(name.data);
        return;
    }

    br_x509_trust_anchor *anchor = &anchors[anchor_count];

    anchor->dn.data = name.data;
    anchor->dn.len = name.length;
    anchor->flags = br_x509_decoder_isCA(&decoder) ? BR_X509_TA_CA : 0;
    anchor->pkey.key_type = key->key_type;

    if (key->key_type == BR_KEYTYPE_RSA)
    {
        anchor->pkey.key.rsa.n = copy_of(key->key.rsa.n, key->key.rsa.nlen);
        anchor->pkey.key.rsa.nlen = key->key.rsa.nlen;
        anchor->pkey.key.rsa.e = copy_of(key->key.rsa.e, key->key.rsa.elen);
        anchor->pkey.key.rsa.elen = key->key.rsa.elen;
    }
    else if (key->key_type == BR_KEYTYPE_EC)
    {
        anchor->pkey.key.ec.curve = key->key.ec.curve;
        anchor->pkey.key.ec.q = copy_of(key->key.ec.q, key->key.ec.qlen);
        anchor->pkey.key.ec.qlen = key->key.ec.qlen;
    }
    else
    {
        free(name.data);
        return;
    }

    anchor_count++;
}

static void load_anchors(void)
{
    anchors_loaded = 1;
    anchors = calloc(ANCHORS_MAX, sizeof(br_x509_trust_anchor));

    FILE *file = fopen(CERTS_FILE, "r");

    if (!anchors || !file)
    {
        if (file)
        {
            fclose(file);
        }

        return;
    }

    unsigned char *pem = malloc(CERTS_MAX + 1);
    size_t length = pem ? fread(pem, 1, CERTS_MAX, file) : 0;

    fclose(file);

    if (!pem)
    {
        return;
    }

    pem[length++] = '\n';

    br_pem_decoder_context decoder;
    bytes_t der = {0, 0, 0};
    int certificate = 0;
    const unsigned char *p = pem;

    br_pem_decoder_init(&decoder);

    while (length > 0)
    {
        size_t used = br_pem_decoder_push(&decoder, p, length);

        p += used;
        length -= used;

        int event = br_pem_decoder_event(&decoder);

        if (event == BR_PEM_BEGIN_OBJ)
        {
            const char *name = br_pem_decoder_name(&decoder);

            certificate = strcmp(name, "CERTIFICATE") == 0 || strcmp(name, "X509 CERTIFICATE") == 0 ||
                          strcmp(name, "TRUSTED CERTIFICATE") == 0;
            der.length = 0;
            br_pem_decoder_setdest(&decoder, certificate ? append_callback : 0, certificate ? &der : 0);
        }
        else if (event == BR_PEM_END_OBJ)
        {
            if (certificate && der.length > 0)
            {
                add_anchor(der.data, der.length);
            }

            certificate = 0;
        }
        else if (event == BR_PEM_ERROR)
        {
            break;
        }
    }

    free(der.data);
    free(pem);
}

int http_parse_url(const char *text, http_url_t *url)
{
    url->secure = 0;

    if (strncasecmp(text, "https://", 8) == 0)
    {
        url->secure = 1;
        text += 8;
    }
    else if (strncasecmp(text, "http://", 7) == 0)
    {
        text += 7;
    }
    else if (strstr(text, "://"))
    {
        return -1;
    }

    size_t host_length = strcspn(text, "/?#");
    const char *rest = text + host_length;
    const char *colon = memchr(text, ':', host_length);

    url->port = url->secure ? 443 : 80;

    if (colon)
    {
        url->port = (unsigned int)atoi(colon + 1);
        host_length = (size_t)(colon - text);
    }

    if (host_length == 0 || host_length >= sizeof(url->host) || url->port == 0 || url->port > 65535)
    {
        return -1;
    }

    for (size_t i = 0; i < host_length; i++)
    {
        url->host[i] = (char)tolower((unsigned char)text[i]);
    }

    url->host[host_length] = 0;

    size_t path_length = strcspn(rest, "#");

    if (path_length + 2 > sizeof(url->path))
    {
        return -1;
    }

    url->path[0] = '/';
    memcpy(url->path + (*rest == '/' ? 0 : 1), rest, path_length);
    url->path[path_length + (*rest == '/' ? 0 : 1)] = 0;
    return 0;
}

static void format_url(const http_url_t *url, char *out, size_t room)
{
    int default_port = url->port == (url->secure ? 443U : 80U);
    char port[8] = "";

    if (!default_port)
    {
        snprintf(port, sizeof(port), ":%u", url->port);
    }

    snprintf(out, room, "%s://%s%s%s", url->secure ? "https" : "http", url->host, port, url->path);
}

static void remove_dot_segments(char *path)
{
    char *out = path;
    const char *in = path;

    while (*in)
    {
        if (in[0] == '/' && in[1] == '.' && (in[2] == '/' || in[2] == 0 || in[2] == '?'))
        {
            in += 2;

            if (*in != '/')
            {
                *out++ = '/';
            }

            continue;
        }

        if (in[0] == '/' && in[1] == '.' && in[2] == '.' && (in[3] == '/' || in[3] == 0 || in[3] == '?'))
        {
            in += 3;

            while (out > path && *--out != '/')
            {
            }

            if (*in != '/')
            {
                *out++ = '/';
            }

            continue;
        }

        if (*in == '?')
        {
            while (*in)
            {
                *out++ = *in++;
            }

            break;
        }

        *out++ = *in++;
    }

    *out = 0;

    if (path[0] == 0)
    {
        path[0] = '/';
        path[1] = 0;
    }
}

int http_join(const char *base, const char *link, char *out, size_t room)
{
    http_url_t url;

    while (*link == ' ' || *link == '\t' || *link == '\n' || *link == '\r')
    {
        link++;
    }

    if (strncasecmp(link, "http://", 7) == 0 || strncasecmp(link, "https://", 8) == 0)
    {
        if (http_parse_url(link, &url) != 0)
        {
            return -1;
        }

        format_url(&url, out, room);
        return 0;
    }

    if (strchr(link, ':') && strcspn(link, ":") < strcspn(link, "/?#"))
    {
        return -1;
    }

    if (http_parse_url(base, &url) != 0)
    {
        return -1;
    }

    char path[HTTP_URL_MAX * 2];

    if (link[0] == '/' && link[1] == '/')
    {
        snprintf(path, sizeof(path), "%s:%s", url.secure ? "https" : "http", link);

        if (http_parse_url(path, &url) != 0)
        {
            return -1;
        }

        format_url(&url, out, room);
        return 0;
    }

    size_t link_length = strcspn(link, "#");

    if (link_length == 0)
    {
        snprintf(path, sizeof(path), "%s", url.path);
    }
    else if (link[0] == '/')
    {
        snprintf(path, sizeof(path), "%.*s", (int)link_length, link);
    }
    else if (link[0] == '?')
    {
        size_t keep = strcspn(url.path, "?");

        snprintf(path, sizeof(path), "%.*s%.*s", (int)keep, url.path, (int)link_length, link);
    }
    else
    {
        size_t keep = strcspn(url.path, "?");

        while (keep > 0 && url.path[keep - 1] != '/')
        {
            keep--;
        }

        snprintf(path, sizeof(path), "%.*s%.*s", (int)keep, url.path, (int)link_length, link);
    }

    remove_dot_segments(path);

    if (strlen(path) >= sizeof(url.path))
    {
        return -1;
    }

    strcpy(url.path, path);
    format_url(&url, out, room);
    return 0;
}

static int socket_read(void *context, unsigned char *data, size_t length)
{
    connection_t *c = context;
    long got = tcp_recv(c->tcp, data, length);

    if (got <= 0)
    {
        c->tcp_closed = 1;
        return -1;
    }

    return (int)got;
}

static int socket_write(void *context, const unsigned char *data, size_t length)
{
    connection_t *c = context;
    long sent = tcp_send(c->tcp, data, length);

    return sent > 0 ? (int)sent : -1;
}

static void tls_error(int code, http_response_t *response)
{
    const char *text = 0;

    if (code == BR_ERR_X509_NOT_TRUSTED)
    {
        text = "the certificate is not signed by a trusted authority (" CERTS_FILE ")";
    }
    else if (code == BR_ERR_X509_BAD_SERVER_NAME)
    {
        text = "the certificate belongs to a different name";
    }
    else if (code == BR_ERR_X509_EXPIRED)
    {
        text = "the certificate has expired or is not valid yet";
    }
    else if (code == BR_ERR_BAD_VERSION || code == BR_ERR_UNSUPPORTED_VERSION)
    {
        text = "the server didn't answer with https (TLS); try http://";
    }
    else if (code == BR_ERR_RECV_FATAL_ALERT + TLS_ALERT_PROTOCOL_VERSION)
    {
        text = "the server only speaks TLS 1.3, and KnocOS has TLS 1.0 - 1.2 for now";
    }
    else if (code >= BR_ERR_RECV_FATAL_ALERT && code < BR_ERR_SEND_FATAL_ALERT)
    {
        snprintf(response->error, sizeof(response->error), "the server refused the secure connection (alert %d)",
                 code - BR_ERR_RECV_FATAL_ALERT);
        return;
    }
    else if (code == BR_ERR_IO)
    {
        text = "the connection closed during the secure handshake";
    }

    if (text)
    {
        snprintf(response->error, sizeof(response->error), "%s", text);
    }
    else
    {
        snprintf(response->error, sizeof(response->error), "secure connection failed (TLS error %d)", code);
    }
}

static int connection_open(connection_t *c, const http_url_t *url, http_response_t *response)
{
    unsigned int address;

    c->secure = url->secure;
    c->tcp_closed = 0;

    if (net_resolve(url->host, &address) != 0)
    {
        snprintf(response->error, sizeof(response->error), "cannot find %s", url->host);
        return -1;
    }

    unsigned char seed[SEED_BYTES];
    long now = 0;

    if (c->secure)
    {
        if (!anchors_loaded)
        {
            load_anchors();
        }

        if (anchor_count == 0)
        {
            snprintf(response->error, sizeof(response->error), "no trusted certificates in %s", CERTS_FILE);
            return -1;
        }

        now = realtime();

        if (now <= 0)
        {
            snprintf(response->error, sizeof(response->error), "https needs the real time, and there is no clock");
            return -1;
        }

        if (random_bytes(seed, sizeof(seed)) != (long)sizeof(seed))
        {
            snprintf(response->error, sizeof(response->error),
                     "https needs random numbers (QEMU: -device virtio-rng-device)");
            return -1;
        }
    }

    c->tcp = tcp_connect(address, url->port);

    if (c->tcp < 0)
    {
        snprintf(response->error, sizeof(response->error), "cannot connect to %s:%u (%s)", url->host, url->port,
                 c->tcp == E_REFUSED ? "refused" : c->tcp == E_TIMEOUT ? "timed out" : "error");
        return -1;
    }

    if (!c->secure)
    {
        return 0;
    }

    br_ssl_client_init_full(&client, &validator, anchors, anchor_count);
    br_x509_minimal_set_time(&validator, (uint32_t)(now / 86400 + UNIX_TO_DAY_ZERO), (uint32_t)(now % 86400));
    br_ssl_engine_set_buffer(&client.eng, io_buffer, sizeof(io_buffer), 1);
    br_ssl_engine_inject_entropy(&client.eng, seed, sizeof(seed));
    memset(seed, 0, sizeof(seed));

    if (!br_ssl_client_reset(&client, url->host, 0))
    {
        tls_error(br_ssl_engine_last_error(&client.eng), response);
        tcp_close(c->tcp);
        return -1;
    }

    br_sslio_init(&io, &client.eng, socket_read, c, socket_write, c);
    return 0;
}

static int connection_write(connection_t *c, const void *data, size_t length, http_response_t *response)
{
    if (!c->secure)
    {
        const char *p = data;

        while (length > 0)
        {
            long sent = tcp_send(c->tcp, p, length);

            if (sent <= 0)
            {
                snprintf(response->error, sizeof(response->error), "sending the request failed");
                return -1;
            }

            p += sent;
            length -= (size_t)sent;
        }

        return 0;
    }

    if (br_sslio_write_all(&io, data, length) != 0 || br_sslio_flush(&io) != 0)
    {
        tls_error(br_ssl_engine_last_error(&client.eng), response);
        return -1;
    }

    return 0;
}

static long connection_read(connection_t *c, void *data, size_t length, http_response_t *response)
{
    if (!c->secure)
    {
        long got = tcp_recv(c->tcp, data, length);

        if (got < 0)
        {
            snprintf(response->error, sizeof(response->error), "the connection broke");
        }

        return got;
    }

    int got = br_sslio_read(&io, data, length);

    if (got > 0)
    {
        return got;
    }

    int code = br_ssl_engine_last_error(&client.eng);

    if (code == BR_ERR_OK || (code == BR_ERR_IO && c->tcp_closed))
    {
        return 0;
    }

    tls_error(code, response);
    return -1;
}

static void connection_close(connection_t *c)
{
    tcp_close(c->tcp);
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

            while (*value == ' ' || *value == '\t')
            {
                value++;
            }

            while (value[n] && value[n] != '\r' && n < room - 1)
            {
                out[n] = value[n];
                n++;
            }

            while (n > 0 && out[n - 1] == ' ')
            {
                n--;
            }

            out[n] = 0;
            return 1;
        }
    }

    return 0;
}

static int deliver_raw(body_t *b, const char *data, size_t length)
{
    if (length == 0 || b->aborted)
    {
        return 0;
    }

    b->response->received += (long)length;

    if (b->sink && b->sink(b->context, data, length, b->response) != 0)
    {
        b->aborted = 1;
    }

    return 0;
}

static int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9')
    {
        return ch - '0';
    }

    ch = (char)tolower((unsigned char)ch);
    return ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1;
}

static void deliver(body_t *b, const char *data, size_t length)
{
    if (!b->chunked)
    {
        deliver_raw(b, data, length);
        return;
    }

    size_t i = 0;

    while (i < length && b->chunk_state != CHUNK_DONE)
    {
        char ch = data[i];

        if (b->chunk_state == CHUNK_SIZE)
        {
            int value = hex_value(ch);

            if (value >= 0)
            {
                b->chunk_left = b->chunk_left * 16 + value;
            }
            else if (ch == '\n')
            {
                b->chunk_state = b->chunk_left > 0 ? CHUNK_DATA : CHUNK_DONE;
            }
            else if (ch != '\r')
            {
                b->chunk_state = CHUNK_EXTENSION;
            }

            i++;
        }
        else if (b->chunk_state == CHUNK_EXTENSION)
        {
            if (ch == '\n')
            {
                b->chunk_state = b->chunk_left > 0 ? CHUNK_DATA : CHUNK_DONE;
            }

            i++;
        }
        else if (b->chunk_state == CHUNK_DATA)
        {
            size_t take = length - i;

            if ((long)take > b->chunk_left)
            {
                take = (size_t)b->chunk_left;
            }

            deliver_raw(b, data + i, take);
            b->chunk_left -= (long)take;
            i += take;

            if (b->chunk_left == 0)
            {
                b->chunk_state = CHUNK_DATA_END;
            }
        }
        else
        {
            if (ch == '\n')
            {
                b->chunk_state = CHUNK_SIZE;
                b->chunk_left = 0;
            }

            i++;
        }
    }
}

static int is_redirect(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

int http_get(const char *start, http_sink_t sink, void *context, http_response_t *response)
{
    char target[HTTP_URL_MAX];
    http_url_t url;
    connection_t c;

    memset(response, 0, sizeof(*response));
    snprintf(target, sizeof(target), "%s", start);

    for (;;)
    {
        response->status = 0;
        response->content_type[0] = 0;
        response->content_length = -1;
        response->received = 0;

        if (http_parse_url(target, &url) != 0)
        {
            snprintf(response->error, sizeof(response->error), "bad URL %s", target);
            return -1;
        }

        format_url(&url, response->url, sizeof(response->url));

        if (connection_open(&c, &url, response) != 0)
        {
            return -1;
        }

        char host[HTTP_HOST_MAX + 8];

        if (url.port == (url.secure ? 443U : 80U))
        {
            snprintf(host, sizeof(host), "%s", url.host);
        }
        else
        {
            snprintf(host, sizeof(host), "%s:%u", url.host, url.port);
        }

        int request_length = snprintf((char *)buffer, sizeof(buffer),
                                      "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: KnocOS/" KNOCOS_VERSION
                                      "\r\nAccept: text/html, text/plain, */*\r\nAccept-Encoding: identity\r\n"
                                      "Connection: close\r\n\r\n",
                                      url.path, host);

        if (request_length >= (int)sizeof(buffer))
        {
            snprintf(response->error, sizeof(response->error), "the URL is too long");
            connection_close(&c);
            return -1;
        }

        if (connection_write(&c, buffer, (size_t)request_length, response) != 0)
        {
            connection_close(&c);
            return -1;
        }

        size_t header_length = 0;
        char *body = 0;
        long got = 0;

        while (!body && header_length < HEADER_MAX &&
               (got = connection_read(&c, header + header_length, HEADER_MAX - header_length, response)) > 0)
        {
            size_t from = header_length > 3 ? header_length - 3 : 0;

            header_length += (size_t)got;
            header[header_length] = 0;
            body = strstr(header + from, "\r\n\r\n");
        }

        if (!body)
        {
            if (got >= 0 || !response->error[0])
            {
                snprintf(response->error, sizeof(response->error), "no HTTP answer from %s", url.host);
            }

            connection_close(&c);
            return -1;
        }

        size_t body_offset = (size_t)(body - header) + 4;
        char value[HTTP_URL_MAX];

        *body = 0;

        if (sscanf(header, "HTTP/%*s %d", &response->status) != 1)
        {
            snprintf(response->error, sizeof(response->error), "not an HTTP answer from %s", url.host);
            connection_close(&c);
            return -1;
        }

        if (is_redirect(response->status) && header_value("Location", value, sizeof(value)))
        {
            connection_close(&c);

            if (++response->redirects > REDIRECTS_MAX)
            {
                snprintf(response->error, sizeof(response->error), "too many redirects");
                return -1;
            }

            if (http_join(response->url, value, target, sizeof(target)) != 0)
            {
                snprintf(response->error, sizeof(response->error), "bad redirect to %s", value);
                return -1;
            }

            continue;
        }

        body_t b;

        memset(&b, 0, sizeof(b));
        b.sink = sink;
        b.context = context;
        b.response = response;

        if (header_value("Content-Type", value, sizeof(value)))
        {
            snprintf(response->content_type, sizeof(response->content_type), "%s", value);
        }

        if (header_value("Transfer-Encoding", value, sizeof(value)) && strstr(value, "chunked"))
        {
            b.chunked = 1;
        }
        else if (header_value("Content-Length", value, sizeof(value)))
        {
            response->content_length = atol(value);
        }

        deliver(&b, header + body_offset, header_length - body_offset);

        while (!b.aborted && b.chunk_state != CHUNK_DONE &&
               (response->content_length < 0 || response->received < response->content_length) &&
               (got = connection_read(&c, buffer, sizeof(buffer), response)) > 0)
        {
            deliver(&b, (const char *)buffer, (size_t)got);
        }

        connection_close(&c);

        if (b.aborted)
        {
            if (!response->error[0])
            {
                snprintf(response->error, sizeof(response->error), "stopped");
            }

            return -1;
        }

        int short_body = response->content_length >= 0 && response->received < response->content_length;

        if (got < 0 || short_body || (b.chunked && b.chunk_state != CHUNK_DONE))
        {
            if (got >= 0 || !response->error[0])
            {
                snprintf(response->error, sizeof(response->error), "the connection broke after %ld bytes",
                         response->received);
            }

            return -1;
        }

        return 0;
    }
}
