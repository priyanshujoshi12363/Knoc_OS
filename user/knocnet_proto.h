#ifndef KNOC_KNOCNET_PROTO_H
#define KNOC_KNOCNET_PROTO_H

#include <stddef.h>

#define KN_PORT 7000
#define KN_DIR "/etc/knocnet"
#define KN_IDENTITY KN_DIR "/identity"
#define KN_NAME_FILE KN_DIR "/name"
#define KN_PEERS KN_DIR "/peers"
#define KN_PAIRING KN_DIR "/pairing"
#define KN_INBOX "/home/KnocNet"
#define KN_SHARED "/home/Shared/"
#define KN_PUB 65
#define KN_NAME_MAX 32
#define KN_ADDRESS_MAX 64
#define KN_CODE_MAX 16
#define KN_RECORD_MAX 8192
#define KN_PEERS_MAX 32
#define KN_MODE_NORMAL 0
#define KN_MODE_PAIR 1
#define KN_FINGERPRINT 20

typedef struct kn_identity
{
    unsigned char priv[32];
    unsigned char pub[KN_PUB];
    char name[KN_NAME_MAX];
} kn_identity_t;

typedef struct kn_peer
{
    char name[KN_NAME_MAX];
    unsigned char pub[KN_PUB];
    char address[KN_ADDRESS_MAX];
} kn_peer_t;

typedef struct kn_session
{
    int tcp;
    int mode;
    unsigned char send_key[32];
    unsigned char recv_key[32];
    unsigned long send_count;
    unsigned long recv_count;
    unsigned char peer_pub[KN_PUB];
    char peer_name[KN_NAME_MAX];
    unsigned int remote;
} kn_session_t;

typedef int (*kn_trust_t)(const unsigned char *peer_pub, int mode, char *code);

int kn_identity(kn_identity_t *id);
int kn_set_name(const char *name);
void kn_fingerprint(const unsigned char *pub, char *out);
int kn_peer_list(kn_peer_t *out, int max);
int kn_peer_by_name(const char *name, kn_peer_t *peer);
int kn_peer_by_key(const unsigned char *pub, kn_peer_t *peer);
int kn_peer_save(kn_peer_t *peer);
int kn_peer_remove(const char *name);

int kn_connect(kn_session_t *s, const kn_identity_t *id, const char *address, int mode,
               const unsigned char *expect_pub, const char *code, char *error, size_t room);
int kn_accept(kn_session_t *s, const kn_identity_t *id, int tcp, unsigned int remote, kn_trust_t trust,
              char *error, size_t room);
int kn_send(kn_session_t *s, const void *data, size_t length);
int kn_send_text(kn_session_t *s, const char *text);
long kn_recv(kn_session_t *s, void *buffer, size_t room, int patience);
void kn_close(kn_session_t *s);
int kn_selftest(char *report, size_t room);

#endif
