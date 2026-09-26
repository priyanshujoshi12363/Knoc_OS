#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "knocnet_proto.h"
#include "ulib.h"

#define ANSWER_MAX 4096

static kn_identity_t me;
static char pairing_code[KN_CODE_MAX];
static char request[KN_RECORD_MAX + 1];
static char chunk[KN_RECORD_MAX];

static int pairing_open(char *code)
{
    FILE *f = fopen(KN_PAIRING, "r");
    long expires = 0;
    char text[KN_CODE_MAX] = "";

    if (!f)
    {
        return 0;
    }

    char line[64] = "";
    int ok = fgets(line, sizeof(line), f) && sscanf(line, "%15s %ld", text, &expires) == 2;

    fclose(f);

    if (!ok || realtime() > expires)
    {
        remove(KN_PAIRING);
        return 0;
    }

    strcpy(code, text);
    return 1;
}

static int trust(const unsigned char *pub, int mode, char *code)
{
    if (mode == KN_MODE_PAIR)
    {
        if (!pairing_open(pairing_code))
        {
            return 0;
        }

        strcpy(code, pairing_code);
        return 1;
    }

    return kn_peer_by_key(pub, 0) == 0;
}

static void format_ip(unsigned int ip, char *out, size_t room)
{
    snprintf(out, room, "%u.%u.%u.%u:%d", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, KN_PORT);
}

static int safe_name(const char *name)
{
    return name[0] && !strchr(name, '/') && strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

static void reply(kn_session_t *s, const char *text)
{
    kn_send_text(s, text);
}

static void do_status(kn_session_t *s)
{
    system_info_t info;
    char text[512];

    if (sysinfo(&info) != 0)
    {
        reply(s, "ERROR status not available");
        return;
    }

    snprintf(text, sizeof(text),
             "%s: up %lu s, RAM %lu of %lu MiB free, disk %lu of %lu MiB free, AI space %s, "
             "%u kernel crashes, %u warm restarts%s",
             me.name, (unsigned long)(info.uptime_ticks / 100), (unsigned long)(info.ram_free_bytes >> 20),
             (unsigned long)(info.ram_bytes >> 20), (unsigned long)(info.disk_free_bytes >> 20),
             (unsigned long)(info.disk_bytes >> 20), info.ai_online ? "online" : "offline", info.crashes_total,
             info.kernel_restarts, info.safe_mode ? ", SAFE MODE" : "");
    reply(s, text);
}

static void do_receive(kn_session_t *s, const char *args)
{
    long size = 0;
    char name[FILE_NAME_MAX];
    char folder[PATH_MAX];
    char path[PATH_MAX];

    if (sscanf(args, "%ld %59s", &size, name) != 2 || size < 0 || !safe_name(name))
    {
        reply(s, "ERROR bad file name");
        return;
    }

    snprintf(folder, sizeof(folder), "%s/%s", KN_INBOX, s->peer_name);
    mkdir(KN_INBOX);
    mkdir(folder);
    snprintf(path, sizeof(path), "%s/%s", folder, name);

    FILE *out = fopen(path, "w");

    if (!out)
    {
        reply(s, "ERROR cannot write the file");
        return;
    }

    reply(s, "READY");

    long received = 0;

    while (received < size)
    {
        long got = kn_recv(s, chunk, sizeof(chunk), 1);

        if (got <= 0)
        {
            break;
        }

        fwrite(chunk, 1, (size_t)got, out);
        received += got;
    }

    fclose(out);

    if (received != size)
    {
        remove(path);
        printf("[KNOCNET] %s: file %s broke off after %ld bytes\n", s->peer_name, name, received);
        return;
    }

    snprintf(request, sizeof(request), "SAVED %s %ld", path, size);
    reply(s, request);
    printf("[KNOCNET] %s sent %s (%ld bytes) -> %s\n", s->peer_name, name, size, path);
}

static void do_send(kn_session_t *s, const char *requested)
{
    char path[PATH_MAX];

    snprintf(path, sizeof(path), "%s", requested);

    if (strncmp(path, KN_SHARED, strlen(KN_SHARED)) != 0 || strstr(path, ".."))
    {
        reply(s, "ERROR only files in " KN_SHARED " can be fetched");
        printf("[KNOCNET] %s asked for %s: refused (not in %s)\n", s->peer_name, path, KN_SHARED);
        return;
    }

    FILE *in = fopen(path, "r");

    if (!in)
    {
        reply(s, "ERROR no such file");
        return;
    }

    fseek(in, 0, SEEK_END);

    long size = ftell(in);

    fseek(in, 0, SEEK_SET);
    snprintf(request, sizeof(request), "FILE %ld", size);
    reply(s, request);

    size_t got;

    while ((got = fread(chunk, 1, sizeof(chunk), in)) > 0)
    {
        if (kn_send(s, chunk, got) != 0)
        {
            break;
        }
    }

    fclose(in);
    printf("[KNOCNET] %s fetched %s (%ld bytes)\n", s->peer_name, path, size);
}

static void do_ask(kn_session_t *s, const char *question)
{
    static char answer[ANSWER_MAX + 8];

    printf("[KNOCNET] %s asks: %s\n", s->peer_name, question);

    int pid = spawn_capture("ask", question, 1);

    if (pid < 0)
    {
        reply(s, "ERROR cannot start ask here");
        return;
    }

    long code = wait(pid);
    long length = captured(answer + 7, ANSWER_MAX);

    if (length < 0)
    {
        length = 0;
    }

    memcpy(answer, "ANSWER ", 7);
    answer[7 + length] = 0;

    if (length == 0)
    {
        snprintf(answer, sizeof(answer), "ANSWER (ask exited with code %ld and printed nothing)", code);
    }

    reply(s, answer);
}

static void serve(kn_session_t *s)
{
    while (1)
    {
        long got = kn_recv(s, request, sizeof(request) - 1, 2);

        if (got <= 0)
        {
            return;
        }

        request[got] = 0;

        if (strcmp(request, "PING") == 0)
        {
            snprintf(chunk, sizeof(chunk), "PONG %s", me.name);
            reply(s, chunk);
        }
        else if (strcmp(request, "STATUS") == 0)
        {
            do_status(s);
        }
        else if (strncmp(request, "SEND ", 5) == 0)
        {
            do_receive(s, request + 5);
        }
        else if (strncmp(request, "GET ", 4) == 0)
        {
            do_send(s, request + 4);
        }
        else if (strncmp(request, "ASK ", 4) == 0)
        {
            do_ask(s, request + 4);
        }
        else if (strcmp(request, "BYE") == 0)
        {
            return;
        }
        else
        {
            reply(s, "ERROR unknown request");
        }
    }
}

int main(void)
{
    char error[128];
    char fingerprint[KN_FINGERPRINT];

    setvbuf(stdout, 0, _IONBF, 0);

    if (kn_identity(&me) != 0)
    {
        printf("[KNOCNET] cannot create this machine's keys (no random number device?)\n");
        return 1;
    }

    int listener = tcp_listen(KN_PORT);

    if (listener < 0)
    {
        printf("[KNOCNET] cannot listen on port %d (no network?)\n", KN_PORT);
        return 1;
    }

    kn_fingerprint(me.pub, fingerprint);
    printf("[KNOCNET] ready: %s (%s), listening on port %d\n", me.name, fingerprint, KN_PORT);

    while (1)
    {
        unsigned int remote = 0;
        int tcp = tcp_accept(listener, &remote, 0);

        if (tcp < 0)
        {
            continue;
        }

        kn_identity(&me);

        kn_session_t session;

        if (kn_accept(&session, &me, tcp, remote, trust, error, sizeof(error)) != 0)
        {
            kn_fingerprint(session.peer_pub, fingerprint);
            printf("[KNOCNET] %s (%s)\n", error, fingerprint);
            tcp_close(tcp);
            continue;
        }

        if (session.mode == KN_MODE_PAIR)
        {
            kn_peer_t peer;

            memset(&peer, 0, sizeof(peer));
            snprintf(peer.name, sizeof(peer.name), "%s", session.peer_name);
            memcpy(peer.pub, session.peer_pub, KN_PUB);
            format_ip(remote, peer.address, sizeof(peer.address));
            kn_peer_save(&peer);
            remove(KN_PAIRING);
            kn_fingerprint(peer.pub, fingerprint);
            printf("[KNOCNET] paired with %s (%s)\n", peer.name, fingerprint);
        }

        serve(&session);
        kn_close(&session);
    }
}
