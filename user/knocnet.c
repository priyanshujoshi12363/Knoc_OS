#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "knocnet_proto.h"
#include "ulib.h"

#define PAIR_SECONDS 120

static kn_identity_t me;
static char error[160];
static char buffer[KN_RECORD_MAX + 1];

static void usage(void)
{
    printf("usage: knocnet id                       this machine's name and key fingerprint\n"
           "       knocnet name NAME                rename this machine\n"
           "       knocnet pair wait [CODE]         let another KnocOS pair with this one\n"
           "       knocnet pair ADDRESS CODE        pair with a KnocOS that is waiting\n"
           "       knocnet peers                    trusted machines\n"
           "       knocnet unpair NAME              stop trusting a machine\n"
           "       knocnet ping NAME|ADDRESS        check the link\n"
           "       knocnet status NAME              health of the other machine\n"
           "       knocnet send NAME FILE           send a file (lands in " KN_INBOX "/YOURNAME there)\n"
           "       knocnet get NAME PATH [FILE]     fetch a file from " KN_SHARED " there\n"
           "       knocnet ask NAME QUESTION        ask the other machine's AI\n"
           "       knocnet selftest                 check the cryptography\n");
}

static int open_peer(kn_session_t *s, const char *target)
{
    kn_peer_t peer;

    if (kn_peer_by_name(target, &peer) == 0)
    {
        if (kn_connect(s, &me, peer.address, KN_MODE_NORMAL, peer.pub, 0, error, sizeof(error)) != 0)
        {
            printf("knocnet: %s: %s\n", target, error);
            return -1;
        }

        return 0;
    }

    if (!strchr(target, '.') && !strchr(target, ':'))
    {
        printf("knocnet: no machine called %s (see knocnet peers)\n", target);
        return -1;
    }

    if (kn_connect(s, &me, target, KN_MODE_NORMAL, 0, 0, error, sizeof(error)) != 0)
    {
        printf("knocnet: %s: %s\n", target, error);
        return -1;
    }

    if (kn_peer_by_key(s->peer_pub, 0) != 0)
    {
        printf("knocnet: %s is not paired with this machine\n", target);
        kn_close(s);
        return -1;
    }

    return 0;
}

static long request(kn_session_t *s, const char *text, int patience)
{
    if (kn_send_text(s, text) != 0)
    {
        return -1;
    }

    long got = kn_recv(s, buffer, sizeof(buffer) - 1, patience);

    if (got >= 0)
    {
        buffer[got] = 0;
    }

    return got;
}

static void bye(kn_session_t *s)
{
    kn_send_text(s, "BYE");
    kn_close(s);
}

static int cmd_id(void)
{
    char fingerprint[KN_FINGERPRINT];

    kn_fingerprint(me.pub, fingerprint);
    printf("name:        %s\nfingerprint: %s\nport:        %d (knocnetd)\n", me.name, fingerprint, KN_PORT);
    return 0;
}

static int cmd_pair_wait(const char *given)
{
    char code[KN_CODE_MAX];
    kn_peer_t before[KN_PEERS_MAX];
    int count_before = kn_peer_list(before, KN_PEERS_MAX);

    if (given)
    {
        snprintf(code, sizeof(code), "%s", given);
    }
    else
    {
        unsigned char random[4];

        random_bytes(random, sizeof(random));
        snprintf(code, sizeof(code), "%06lu",
                 (((unsigned long)random[0] << 24) | ((unsigned long)random[1] << 16) | ((unsigned long)random[2] << 8) |
                  random[3]) % 1000000);
    }

    long now = realtime();
    FILE *f;

    mkdir(KN_DIR);
    f = fopen(KN_PAIRING, "w");

    if (!f || now <= 0)
    {
        printf("knocnet: cannot open a pairing window\n");
        return 1;
    }

    fprintf(f, "%s %ld\n", code, now + PAIR_SECONDS);
    fclose(f);
    printf("Pairing code: %s (valid %d s). On the other KnocOS run:\n  knocnet pair ADDRESS-OF-THIS-MACHINE %s\n",
           code, PAIR_SECONDS, code);
    printf("waiting...\n");

    while (realtime() <= now + PAIR_SECONDS)
    {
        file_stat_t info;

        if (stat(KN_PAIRING, &info) != 0)
        {
            kn_peer_t after[KN_PEERS_MAX];
            int count = kn_peer_list(after, KN_PEERS_MAX);
            char fingerprint[KN_FINGERPRINT];

            if (count > 0 && count >= count_before)
            {
                kn_fingerprint(after[count - 1].pub, fingerprint);
                printf("paired with %s (%s)\n", after[count - 1].name, fingerprint);
                return 0;
            }

            printf("knocnet: the pairing window was closed\n");
            return 1;
        }

        sleep(50);
    }

    remove(KN_PAIRING);
    printf("knocnet: nobody paired within %d s\n", PAIR_SECONDS);
    return 1;
}

static int cmd_pair(const char *address, const char *code)
{
    kn_session_t s;
    kn_peer_t peer;
    char fingerprint[KN_FINGERPRINT];

    if (kn_connect(&s, &me, address, KN_MODE_PAIR, 0, code, error, sizeof(error)) != 0)
    {
        printf("knocnet: pairing failed: %s\n", error);
        return 1;
    }

    memset(&peer, 0, sizeof(peer));
    snprintf(peer.name, sizeof(peer.name), "%s", s.peer_name);
    memcpy(peer.pub, s.peer_pub, KN_PUB);
    snprintf(peer.address, sizeof(peer.address), "%s", address);

    if (!strchr(peer.address, ':'))
    {
        snprintf(peer.address + strlen(peer.address), sizeof(peer.address) - strlen(peer.address), ":%d", KN_PORT);
    }

    bye(&s);

    if (kn_peer_save(&peer) != 0)
    {
        printf("knocnet: cannot save the peer\n");
        return 1;
    }

    kn_fingerprint(peer.pub, fingerprint);
    printf("paired with %s (%s) at %s\n", peer.name, fingerprint, peer.address);
    return 0;
}

static int cmd_peers(void)
{
    kn_peer_t peers[KN_PEERS_MAX];
    int count = kn_peer_list(peers, KN_PEERS_MAX);

    if (count == 0)
    {
        printf("no trusted machines yet (knocnet pair)\n");
        return 0;
    }

    for (int i = 0; i < count; i++)
    {
        char fingerprint[KN_FINGERPRINT];

        kn_fingerprint(peers[i].pub, fingerprint);
        printf("%-16s %s  %s\n", peers[i].name, fingerprint, peers[i].address);
    }

    return 0;
}

static int cmd_ping(const char *target)
{
    kn_session_t s;
    unsigned long started = uptime();

    if (open_peer(&s, target) != 0)
    {
        return 1;
    }

    long got = request(&s, "PING", 1);
    unsigned long ms = (uptime() - started) * 10;

    bye(&s);

    if (got <= 0 || strncmp(buffer, "PONG ", 5) != 0)
    {
        printf("knocnet: no answer\n");
        return 1;
    }

    printf("%s answered over an encrypted link in %lu ms\n", buffer + 5, ms);
    return 0;
}

static int cmd_status(const char *target)
{
    kn_session_t s;

    if (open_peer(&s, target) != 0)
    {
        return 1;
    }

    long got = request(&s, "STATUS", 1);

    bye(&s);

    if (got <= 0)
    {
        printf("knocnet: no answer\n");
        return 1;
    }

    printf("%s\n", buffer);
    return strncmp(buffer, "ERROR", 5) == 0;
}

static int cmd_send(const char *target, const char *path)
{
    kn_session_t s;
    FILE *in = fopen(path, "r");

    if (!in)
    {
        printf("knocnet: cannot read %s\n", path);
        return 1;
    }

    fseek(in, 0, SEEK_END);

    long size = ftell(in);
    const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;

    fseek(in, 0, SEEK_SET);

    if (open_peer(&s, target) != 0)
    {
        fclose(in);
        return 1;
    }

    char line[128];

    snprintf(line, sizeof(line), "SEND %ld %s", size, name);

    if (request(&s, line, 1) <= 0 || strcmp(buffer, "READY") != 0)
    {
        printf("knocnet: %s refused: %s\n", target, buffer);
        fclose(in);
        bye(&s);
        return 1;
    }

    size_t got;

    while ((got = fread(buffer, 1, KN_RECORD_MAX, in)) > 0)
    {
        if (kn_send(&s, buffer, got) != 0)
        {
            break;
        }
    }

    fclose(in);

    long answer = kn_recv(&s, buffer, sizeof(buffer) - 1, 2);

    bye(&s);

    if (answer <= 0)
    {
        printf("knocnet: the transfer broke off\n");
        return 1;
    }

    buffer[answer] = 0;
    printf("sent %s (%ld bytes) to %s: %s\n", name, size, target, buffer);
    return strncmp(buffer, "SAVED", 5) != 0;
}

static int cmd_get(const char *target, const char *path, const char *local)
{
    kn_session_t s;
    char line[PATH_MAX + 8];
    long size = 0;

    if (!local)
    {
        local = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    }

    if (open_peer(&s, target) != 0)
    {
        return 1;
    }

    snprintf(line, sizeof(line), "GET %s", path);

    if (request(&s, line, 1) <= 0 || sscanf(buffer, "FILE %ld", &size) != 1)
    {
        printf("knocnet: %s: %s\n", target, buffer[0] ? buffer : "no answer");
        bye(&s);
        return 1;
    }

    FILE *out = fopen(local, "w");

    if (!out)
    {
        printf("knocnet: cannot write %s\n", local);
        bye(&s);
        return 1;
    }

    long received = 0;

    while (received < size)
    {
        long got = kn_recv(&s, buffer, KN_RECORD_MAX, 1);

        if (got <= 0)
        {
            break;
        }

        fwrite(buffer, 1, (size_t)got, out);
        received += got;
    }

    fclose(out);
    bye(&s);

    if (received != size)
    {
        remove(local);
        printf("knocnet: the transfer broke off after %ld bytes\n", received);
        return 1;
    }

    printf("got %s from %s: %ld bytes saved to %s\n", path, target, size, local);
    return 0;
}

static int cmd_ask(const char *target, int argc, char **argv)
{
    kn_session_t s;
    char question[1024] = "ASK ";

    for (int i = 0; i < argc; i++)
    {
        snprintf(question + strlen(question), sizeof(question) - strlen(question), "%s%s", i ? " " : "", argv[i]);
    }

    if (open_peer(&s, target) != 0)
    {
        return 1;
    }

    printf("asking %s's AI (this can take a while)...\n", target);

    long got = request(&s, question, 40);

    bye(&s);

    if (got <= 0 || strncmp(buffer, "ANSWER ", 7) != 0)
    {
        printf("knocnet: %s\n", got > 0 ? buffer : "no answer");
        return 1;
    }

    printf("%s says:\n%s\n", target, buffer + 7);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        usage();
        return 1;
    }

    const char *command = argv[1];

    if (strcmp(command, "selftest") == 0)
    {
        char report[512];
        int result = kn_selftest(report, sizeof(report));

        printf("%sknocnet selftest: %s\n", report, result == 0 ? "all checks passed" : "FAILED");
        return result != 0;
    }

    if (kn_identity(&me) != 0)
    {
        printf("knocnet: cannot create this machine's keys (no random number device?)\n");
        return 1;
    }

    if (strcmp(command, "id") == 0)
    {
        return cmd_id();
    }

    if (strcmp(command, "name") == 0 && argc == 3)
    {
        if (kn_set_name(argv[2]) != 0)
        {
            printf("knocnet: a name is one word, up to %d letters\n", KN_NAME_MAX - 1);
            return 1;
        }

        printf("this machine is now called %s\n", argv[2]);
        return 0;
    }

    if (strcmp(command, "pair") == 0 && argc >= 3 && strcmp(argv[2], "wait") == 0)
    {
        return cmd_pair_wait(argc > 3 ? argv[3] : 0);
    }

    if (strcmp(command, "pair") == 0 && argc == 4)
    {
        return cmd_pair(argv[2], argv[3]);
    }

    if (strcmp(command, "peers") == 0)
    {
        return cmd_peers();
    }

    if (strcmp(command, "unpair") == 0 && argc == 3)
    {
        if (kn_peer_remove(argv[2]) != 0)
        {
            printf("knocnet: no machine called %s\n", argv[2]);
            return 1;
        }

        printf("%s is no longer trusted\n", argv[2]);
        return 0;
    }

    if (strcmp(command, "ping") == 0 && argc == 3)
    {
        return cmd_ping(argv[2]);
    }

    if (strcmp(command, "status") == 0 && argc == 3)
    {
        return cmd_status(argv[2]);
    }

    if (strcmp(command, "send") == 0 && argc == 4)
    {
        return cmd_send(argv[2], argv[3]);
    }

    if (strcmp(command, "get") == 0 && (argc == 4 || argc == 5))
    {
        return cmd_get(argv[2], argv[3], argc == 5 ? argv[4] : 0);
    }

    if (strcmp(command, "ask") == 0 && argc >= 4)
    {
        return cmd_ask(argv[2], argc - 3, argv + 3);
    }

    usage();
    return 1;
}
