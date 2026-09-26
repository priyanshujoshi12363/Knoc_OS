#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bearssl.h"
#include "knocnet_proto.h"
#include "ulib.h"

#define MAGIC "KNOCNET1"
#define HELLO_SIZE (8 + 1 + KN_PUB + 32 + 16)
#define PROOF_SIZE (64 + 32)
#define STATUS_OK 0
#define STATUS_UNTRUSTED 1
#define STATUS_NOT_PAIRING 2

typedef struct keypair
{
    unsigned char secret[BR_EC_KBUF_PRIV_MAX_SIZE];
    size_t secret_length;
    unsigned char pub[BR_EC_KBUF_PUB_MAX_SIZE];
    size_t pub_length;
} keypair_t;

static const br_ec_impl *curves(void)
{
    return br_ec_get_default();
}

static int seeded_drbg(br_hmac_drbg_context *drbg)
{
    unsigned char seed[32];

    if (random_bytes(seed, sizeof(seed)) != (long)sizeof(seed))
    {
        return -1;
    }

    br_hmac_drbg_init(drbg, &br_sha256_vtable, seed, sizeof(seed));
    memset(seed, 0, sizeof(seed));
    return 0;
}

static int make_keypair(int curve, keypair_t *k)
{
    br_hmac_drbg_context drbg;
    br_ec_private_key sk;

    if (seeded_drbg(&drbg) != 0)
    {
        return -1;
    }

    k->secret_length = br_ec_keygen(&drbg.vtable, curves(), &sk, k->secret, curve);

    if (k->secret_length == 0)
    {
        return -1;
    }

    k->pub_length = br_ec_compute_pub(curves(), 0, k->pub, &sk);
    return k->pub_length ? 0 : -1;
}

static void hex(const unsigned char *data, size_t length, char *out)
{
    static const char digits[] = "0123456789abcdef";

    for (size_t i = 0; i < length; i++)
    {
        out[i * 2] = digits[data[i] >> 4];
        out[i * 2 + 1] = digits[data[i] & 15];
    }

    out[length * 2] = 0;
}

static int unhex(const char *text, unsigned char *out, size_t length)
{
    for (size_t i = 0; i < length; i++)
    {
        int value = 0;

        for (int j = 0; j < 2; j++)
        {
            char ch = text[i * 2 + j];

            value <<= 4;

            if (ch >= '0' && ch <= '9')
            {
                value |= ch - '0';
            }
            else if (ch >= 'a' && ch <= 'f')
            {
                value |= ch - 'a' + 10;
            }
            else
            {
                return -1;
            }
        }

        out[i] = (unsigned char)value;
    }

    return 0;
}

void kn_fingerprint(const unsigned char *pub, char *out)
{
    br_sha256_context sha;
    unsigned char hash[32];
    char text[17];

    br_sha256_init(&sha);
    br_sha256_update(&sha, pub, KN_PUB);
    br_sha256_out(&sha, hash);
    hex(hash, 8, text);
    snprintf(out, KN_FINGERPRINT, "%.4s-%.4s-%.4s-%.4s", text, text + 4, text + 8, text + 12);
}

static void read_name(kn_identity_t *id)
{
    FILE *f = fopen(KN_NAME_FILE, "r");
    char fingerprint[KN_FINGERPRINT];

    id->name[0] = 0;

    if (f)
    {
        if (fgets(id->name, sizeof(id->name), f))
        {
            id->name[strcspn(id->name, "\r\n ")] = 0;
        }

        fclose(f);
    }

    if (!id->name[0])
    {
        kn_fingerprint(id->pub, fingerprint);
        snprintf(id->name, sizeof(id->name), "knocos-%.4s", fingerprint);
    }
}

int kn_identity(kn_identity_t *id)
{
    FILE *f = fopen(KN_IDENTITY, "r");

    if (f)
    {
        int ok = fread(id->priv, 1, 32, f) == 32 && fread(id->pub, 1, KN_PUB, f) == KN_PUB;

        fclose(f);

        if (ok)
        {
            read_name(id);
            return 0;
        }
    }

    keypair_t k;

    if (make_keypair(BR_EC_secp256r1, &k) != 0 || k.secret_length != 32 || k.pub_length != KN_PUB)
    {
        return -1;
    }

    memcpy(id->priv, k.secret, 32);
    memcpy(id->pub, k.pub, KN_PUB);
    mkdir(KN_DIR);
    f = fopen(KN_IDENTITY, "w");

    if (!f)
    {
        return -1;
    }

    fwrite(id->priv, 1, 32, f);
    fwrite(id->pub, 1, KN_PUB, f);
    fclose(f);
    read_name(id);
    return 0;
}

int kn_set_name(const char *name)
{
    if (!name[0] || strlen(name) >= KN_NAME_MAX || strchr(name, ' '))
    {
        return -1;
    }

    mkdir(KN_DIR);

    FILE *f = fopen(KN_NAME_FILE, "w");

    if (!f)
    {
        return -1;
    }

    fprintf(f, "%s\n", name);
    fclose(f);
    return 0;
}

int kn_peer_list(kn_peer_t *out, int max)
{
    FILE *f = fopen(KN_PEERS, "r");
    char line[KN_NAME_MAX + KN_PUB * 2 + KN_ADDRESS_MAX + 8];
    char pub[KN_PUB * 2 + 1];
    int count = 0;

    if (!f)
    {
        return 0;
    }

    while (count < max && fgets(line, sizeof(line), f))
    {
        kn_peer_t *p = &out[count];

        p->address[0] = 0;

        if (sscanf(line, "%31s %130s %63s", p->name, pub, p->address) >= 2 && strlen(pub) == KN_PUB * 2 &&
            unhex(pub, p->pub, KN_PUB) == 0)
        {
            count++;
        }
    }

    fclose(f);
    return count;
}

static int write_peers(const kn_peer_t *peers, int count)
{
    char pub[KN_PUB * 2 + 1];

    mkdir(KN_DIR);

    FILE *f = fopen(KN_PEERS, "w");

    if (!f)
    {
        return -1;
    }

    for (int i = 0; i < count; i++)
    {
        hex(peers[i].pub, KN_PUB, pub);
        fprintf(f, "%s %s %s\n", peers[i].name, pub, peers[i].address[0] ? peers[i].address : "-");
    }

    fclose(f);
    return 0;
}

int kn_peer_by_name(const char *name, kn_peer_t *peer)
{
    kn_peer_t peers[KN_PEERS_MAX];
    int count = kn_peer_list(peers, KN_PEERS_MAX);

    for (int i = 0; i < count; i++)
    {
        if (strcmp(peers[i].name, name) == 0)
        {
            *peer = peers[i];
            return 0;
        }
    }

    return -1;
}

int kn_peer_by_key(const unsigned char *pub, kn_peer_t *peer)
{
    kn_peer_t peers[KN_PEERS_MAX];
    int count = kn_peer_list(peers, KN_PEERS_MAX);

    for (int i = 0; i < count; i++)
    {
        if (memcmp(peers[i].pub, pub, KN_PUB) == 0)
        {
            if (peer)
            {
                *peer = peers[i];
            }

            return 0;
        }
    }

    return -1;
}

int kn_peer_save(kn_peer_t *peer)
{
    kn_peer_t peers[KN_PEERS_MAX];
    int count = kn_peer_list(peers, KN_PEERS_MAX);
    int kept = 0;

    for (int i = 0; i < count; i++)
    {
        if (memcmp(peers[i].pub, peer->pub, KN_PUB) != 0)
        {
            peers[kept++] = peers[i];
        }
    }

    for (int i = 0; i < kept; i++)
    {
        if (strcmp(peers[i].name, peer->name) == 0)
        {
            size_t length = strlen(peer->name);

            snprintf(peer->name + (length > KN_NAME_MAX - 3 ? KN_NAME_MAX - 3 : length), 3, "-2");
            i = -1;
        }
    }

    if (kept >= KN_PEERS_MAX)
    {
        return -1;
    }

    peers[kept++] = *peer;
    return write_peers(peers, kept);
}

int kn_peer_remove(const char *name)
{
    kn_peer_t peers[KN_PEERS_MAX];
    int count = kn_peer_list(peers, KN_PEERS_MAX);
    int kept = 0;

    for (int i = 0; i < count; i++)
    {
        if (strcmp(peers[i].name, name) != 0)
        {
            peers[kept++] = peers[i];
        }
    }

    if (kept == count)
    {
        return -1;
    }

    return write_peers(peers, kept);
}

static int read_exact(int tcp, void *buffer, size_t length, int patience)
{
    unsigned char *p = buffer;
    size_t got = 0;
    int waits = 0;

    while (got < length)
    {
        long n = tcp_recv(tcp, p + got, length - got);

        if (n == E_TIMEOUT && ++waits <= patience)
        {
            continue;
        }

        if (n <= 0)
        {
            return -1;
        }

        got += (size_t)n;
    }

    return 0;
}

static int write_all(int tcp, const void *buffer, size_t length)
{
    const unsigned char *p = buffer;

    while (length > 0)
    {
        long n = tcp_send(tcp, p, length);

        if (n <= 0)
        {
            return -1;
        }

        p += n;
        length -= (size_t)n;
    }

    return 0;
}

static void transcript(const unsigned char *hello1, const unsigned char *hello2, unsigned char *out)
{
    br_sha256_context sha;

    br_sha256_init(&sha);
    br_sha256_update(&sha, hello1, HELLO_SIZE);
    br_sha256_update(&sha, hello2, HELLO_SIZE);
    br_sha256_out(&sha, out);
}

static void proof(const char *code, const unsigned char *hash, char role, unsigned char *out)
{
    br_hmac_key_context key;
    br_hmac_context mac;

    br_hmac_key_init(&key, &br_sha256_vtable, code, strlen(code));
    br_hmac_init(&mac, &key, 0);
    br_hmac_update(&mac, hash, 32);
    br_hmac_update(&mac, &role, 1);
    br_hmac_out(&mac, out);
}

static int sign(const kn_identity_t *id, const unsigned char *hash, unsigned char *signature)
{
    br_ec_private_key sk;

    sk.curve = BR_EC_secp256r1;
    sk.x = (unsigned char *)id->priv;
    sk.xlen = 32;
    return br_ecdsa_i31_sign_raw(curves(), &br_sha256_vtable, hash, &sk, signature) == 64 ? 0 : -1;
}

static int verify(const unsigned char *pub, const unsigned char *hash, const unsigned char *signature)
{
    br_ec_public_key pk;

    pk.curve = BR_EC_secp256r1;
    pk.q = (unsigned char *)pub;
    pk.qlen = KN_PUB;
    return br_ecdsa_i31_vrfy_raw(curves(), hash, 32, &pk, signature, 64) ? 0 : -1;
}

static int derive_keys(kn_session_t *s, const keypair_t *ephemeral, const unsigned char *peer_ephemeral,
                       const unsigned char *hash, int client)
{
    unsigned char shared[32];
    unsigned char keys[64];
    br_hkdf_context hkdf;

    memcpy(shared, peer_ephemeral, 32);

    if (!curves()->mul(shared, 32, ephemeral->secret, ephemeral->secret_length, BR_EC_curve25519))
    {
        return -1;
    }

    br_hkdf_init(&hkdf, &br_sha256_vtable, hash, 32);
    br_hkdf_inject(&hkdf, shared, 32);
    br_hkdf_flip(&hkdf);
    br_hkdf_produce(&hkdf, "knocnet v1", 10, keys, sizeof(keys));
    memcpy(client ? s->send_key : s->recv_key, keys, 32);
    memcpy(client ? s->recv_key : s->send_key, keys + 32, 32);
    s->send_count = 0;
    s->recv_count = 0;
    memset(shared, 0, sizeof(shared));
    memset(keys, 0, sizeof(keys));
    return 0;
}

static void make_hello(unsigned char *hello, unsigned char flag, const unsigned char *pub, const keypair_t *ephemeral)
{
    memcpy(hello, MAGIC, 8);
    hello[8] = flag;
    memcpy(hello + 9, pub, KN_PUB);
    memcpy(hello + 9 + KN_PUB, ephemeral->pub, 32);
    random_bytes(hello + 9 + KN_PUB + 32, 16);
}

static int fail(char *error, size_t room, const char *text)
{
    snprintf(error, room, "%s", text);
    return -1;
}

static int exchange_names(kn_session_t *s, const kn_identity_t *id, int client)
{
    char line[64];

    if (client && kn_send_text(s, id->name) != 0)
    {
        return -1;
    }

    long got = kn_recv(s, line, sizeof(line) - 1, 1);

    if (got <= 0)
    {
        return -1;
    }

    line[got] = 0;
    snprintf(s->peer_name, sizeof(s->peer_name), "%s", line);
    s->peer_name[strcspn(s->peer_name, " \r\n")] = 0;
    return client ? 0 : kn_send_text(s, id->name);
}

int kn_connect(kn_session_t *s, const kn_identity_t *id, const char *address, int mode,
               const unsigned char *expect_pub, const char *code, char *error, size_t room)
{
    char host[KN_ADDRESS_MAX];
    unsigned int port = KN_PORT;
    unsigned int ip;
    keypair_t ephemeral;
    unsigned char hello1[HELLO_SIZE];
    unsigned char hello2[HELLO_SIZE];
    unsigned char hash[32];
    unsigned char message[PROOF_SIZE];
    unsigned char expected[32];

    memset(s, 0, sizeof(*s));
    snprintf(host, sizeof(host), "%s", address);

    char *colon = strchr(host, ':');

    if (colon)
    {
        *colon = 0;
        port = (unsigned int)atoi(colon + 1);
    }

    if (net_resolve(host, &ip) != 0)
    {
        return fail(error, room, "cannot find that machine");
    }

    if (make_keypair(BR_EC_curve25519, &ephemeral) != 0)
    {
        return fail(error, room, "no random numbers for the keys");
    }

    s->tcp = tcp_connect(ip, port);

    if (s->tcp < 0)
    {
        return fail(error, room, s->tcp == E_REFUSED ? "connection refused (is knocnetd running there?)"
                                                     : "cannot connect");
    }

    s->mode = mode;
    make_hello(hello1, (unsigned char)mode, id->pub, &ephemeral);

    if (write_all(s->tcp, hello1, HELLO_SIZE) != 0 || read_exact(s->tcp, hello2, HELLO_SIZE, 1) != 0 ||
        memcmp(hello2, MAGIC, 8) != 0)
    {
        kn_close(s);
        return fail(error, room, "the other side doesn't speak KnocNet");
    }

    if (hello2[8] == STATUS_UNTRUSTED)
    {
        kn_close(s);
        return fail(error, room, "refused: that machine doesn't trust this one (pair first)");
    }

    if (hello2[8] == STATUS_NOT_PAIRING)
    {
        kn_close(s);
        return fail(error, room, "refused: that machine isn't waiting for a pairing (run knocnet pair wait there)");
    }

    memcpy(s->peer_pub, hello2 + 9, KN_PUB);

    if (expect_pub && memcmp(expect_pub, s->peer_pub, KN_PUB) != 0)
    {
        kn_close(s);
        return fail(error, room, "WARNING: that machine has a different key than when you paired; not connecting");
    }

    transcript(hello1, hello2, hash);

    if (read_exact(s->tcp, message, PROOF_SIZE, 1) != 0 || verify(s->peer_pub, hash, message) != 0)
    {
        kn_close(s);
        return fail(error, room, "the other machine could not prove its identity");
    }

    if (mode == KN_MODE_PAIR)
    {
        proof(code, hash, 'S', expected);

        if (memcmp(expected, message + 64, 32) != 0)
        {
            kn_close(s);
            return fail(error, room, "wrong pairing code");
        }
    }

    if (sign(id, hash, message) != 0)
    {
        kn_close(s);
        return fail(error, room, "signing failed");
    }

    memset(message + 64, 0, 32);

    if (mode == KN_MODE_PAIR)
    {
        proof(code, hash, 'C', message + 64);
    }

    if (write_all(s->tcp, message, PROOF_SIZE) != 0 || derive_keys(s, &ephemeral, hello2 + 9 + KN_PUB, hash, 1) != 0 ||
        exchange_names(s, id, 1) != 0)
    {
        kn_close(s);
        return fail(error, room, mode == KN_MODE_PAIR ? "the other machine refused the pairing (wrong code?)"
                                                      : "the secure connection failed");
    }

    s->remote = ip;
    return 0;
}

int kn_accept(kn_session_t *s, const kn_identity_t *id, int tcp, unsigned int remote, kn_trust_t trust,
              char *error, size_t room)
{
    keypair_t ephemeral;
    unsigned char hello1[HELLO_SIZE];
    unsigned char hello2[HELLO_SIZE];
    unsigned char hash[32];
    unsigned char message[PROOF_SIZE];
    unsigned char expected[32];
    char code[KN_CODE_MAX] = "";

    memset(s, 0, sizeof(*s));
    s->tcp = tcp;
    s->remote = remote;

    if (read_exact(tcp, hello1, HELLO_SIZE, 0) != 0 || memcmp(hello1, MAGIC, 8) != 0 || hello1[8] > KN_MODE_PAIR)
    {
        return fail(error, room, "not a KnocNet client");
    }

    s->mode = hello1[8];
    memcpy(s->peer_pub, hello1 + 9, KN_PUB);

    if (make_keypair(BR_EC_curve25519, &ephemeral) != 0)
    {
        return fail(error, room, "no random numbers for the keys");
    }

    int allowed = trust(s->peer_pub, s->mode, code);
    unsigned char status = allowed ? STATUS_OK : s->mode == KN_MODE_PAIR ? STATUS_NOT_PAIRING : STATUS_UNTRUSTED;

    make_hello(hello2, status, id->pub, &ephemeral);

    if (write_all(tcp, hello2, HELLO_SIZE) != 0 || !allowed)
    {
        return fail(error, room, status == STATUS_UNTRUSTED ? "refused an unknown machine" : "refused a pairing (not waiting)");
    }

    transcript(hello1, hello2, hash);

    if (sign(id, hash, message) != 0)
    {
        return fail(error, room, "signing failed");
    }

    memset(message + 64, 0, 32);

    if (s->mode == KN_MODE_PAIR)
    {
        proof(code, hash, 'S', message + 64);
    }

    if (write_all(tcp, message, PROOF_SIZE) != 0 || read_exact(tcp, message, PROOF_SIZE, 0) != 0)
    {
        return fail(error, room, "the other machine hung up");
    }

    if (verify(s->peer_pub, hash, message) != 0)
    {
        return fail(error, room, "the other machine could not prove its identity");
    }

    if (s->mode == KN_MODE_PAIR)
    {
        proof(code, hash, 'C', expected);

        if (memcmp(expected, message + 64, 32) != 0)
        {
            return fail(error, room, "wrong pairing code");
        }
    }

    if (derive_keys(s, &ephemeral, hello1 + 9 + KN_PUB, hash, 0) != 0 || exchange_names(s, id, 0) != 0)
    {
        return fail(error, room, "the secure connection failed");
    }

    return 0;
}

static void nonce(unsigned long count, unsigned char *iv)
{
    memset(iv, 0, 12);

    for (int i = 0; i < 8; i++)
    {
        iv[11 - i] = (unsigned char)(count >> (8 * i));
    }
}

int kn_send(kn_session_t *s, const void *data, size_t length)
{
    static unsigned char record[2 + KN_RECORD_MAX + 16];
    const unsigned char *p = data;

    do
    {
        size_t part = length > KN_RECORD_MAX ? KN_RECORD_MAX : length;
        unsigned char iv[12];

        record[0] = (unsigned char)(part >> 8);
        record[1] = (unsigned char)part;
        memcpy(record + 2, p, part);
        nonce(s->send_count++, iv);
        br_poly1305_ctmul_run(s->send_key, iv, record + 2, part, record, 2, record + 2 + part, br_chacha20_ct_run, 1);

        if (write_all(s->tcp, record, 2 + part + 16) != 0)
        {
            return -1;
        }

        p += part;
        length -= part;
    } while (length > 0);

    return 0;
}

int kn_send_text(kn_session_t *s, const char *text)
{
    return kn_send(s, text, strlen(text));
}

long kn_recv(kn_session_t *s, void *buffer, size_t room, int patience)
{
    static unsigned char record[KN_RECORD_MAX + 16];
    unsigned char header[2];
    unsigned char iv[12];
    unsigned char tag[16];

    if (read_exact(s->tcp, header, 2, patience) != 0)
    {
        return -1;
    }

    size_t length = ((size_t)header[0] << 8) | header[1];

    if (length > KN_RECORD_MAX || read_exact(s->tcp, record, length + 16, 1) != 0)
    {
        return -1;
    }

    nonce(s->recv_count++, iv);
    br_poly1305_ctmul_run(s->recv_key, iv, record, length, header, 2, tag, br_chacha20_ct_run, 0);

    unsigned char difference = 0;

    for (int i = 0; i < 16; i++)
    {
        difference |= tag[i] ^ record[length + i];
    }

    if (difference != 0 || length > room)
    {
        return -2;
    }

    memcpy(buffer, record, length);
    return (long)length;
}

void kn_close(kn_session_t *s)
{
    if (s->tcp > 0)
    {
        tcp_close(s->tcp);
        s->tcp = 0;
    }

    memset(s->send_key, 0, sizeof(s->send_key));
    memset(s->recv_key, 0, sizeof(s->recv_key));
}

int kn_selftest(char *report, size_t room)
{
    keypair_t a;
    keypair_t b;
    unsigned char ab[32];
    unsigned char ba[32];
    kn_identity_t id;
    unsigned char hash[32] = {1, 2, 3};
    unsigned char signature[64];
    int failures = 0;
    size_t used = 0;

    if (make_keypair(BR_EC_curve25519, &a) != 0 || make_keypair(BR_EC_curve25519, &b) != 0)
    {
        return fail(report, room, "no random numbers");
    }

    memcpy(ab, b.pub, 32);
    memcpy(ba, a.pub, 32);
    curves()->mul(ab, 32, a.secret, a.secret_length, BR_EC_curve25519);
    curves()->mul(ba, 32, b.secret, b.secret_length, BR_EC_curve25519);

    int agree = memcmp(ab, ba, 32) == 0;

    failures += !agree;
    used += (size_t)snprintf(report + used, room - used, "key exchange (X25519): %s\n", agree ? "both sides agree" : "FAILED");

    keypair_t k;

    make_keypair(BR_EC_secp256r1, &k);
    memcpy(id.priv, k.secret, 32);
    memcpy(id.pub, k.pub, KN_PUB);
    sign(&id, hash, signature);

    int good = verify(id.pub, hash, signature) == 0;

    signature[10] ^= 1;

    int forged = verify(id.pub, hash, signature) == 0;

    failures += !good || forged;
    used += (size_t)snprintf(report + used, room - used, "signatures (ECDSA P-256): %s, a changed signature is %s\n",
                             good ? "valid" : "FAILED", forged ? "ACCEPTED" : "rejected");

    unsigned char key[32];
    unsigned char iv[12] = {0};
    unsigned char text[32] = "hello from knocnet";
    unsigned char copy[32];
    unsigned char tag[16];
    unsigned char check[16];

    memcpy(key, ab, 32);
    memcpy(copy, text, 32);
    br_poly1305_ctmul_run(key, iv, copy, 32, 0, 0, tag, br_chacha20_ct_run, 1);

    int hidden = memcmp(copy, text, 32) != 0;

    copy[5] ^= 0x40;
    br_poly1305_ctmul_run(key, iv, copy, 32, 0, 0, check, br_chacha20_ct_run, 0);

    int tampered = memcmp(check, tag, 16) != 0;

    failures += !hidden || !tampered;
    used += (size_t)snprintf(report + used, room - used, "encryption (ChaCha20-Poly1305): %s, a changed message is %s\n",
                             hidden ? "encrypted" : "FAILED", tampered ? "rejected" : "ACCEPTED");
    return failures ? -1 : 0;
}
