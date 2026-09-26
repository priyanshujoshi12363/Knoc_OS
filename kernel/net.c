#include "net.h"
#include "virtio_net.h"
#include "process.h"
#include "spinlock.h"
#include "timer.h"
#include "logging.h"
#include "knocfs.h"
#include "heap.h"

#define ETH_ARP 0x0806
#define ETH_IP 0x0800
#define IP_ICMP 1
#define IP_TCP 6
#define IP_UDP 17
#define ICMP_ECHO_REPLY 0
#define ICMP_ECHO_REQUEST 8
#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

#define ADDRESS(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
#define LOCAL_IP ADDRESS(10, 0, 2, 15)
#define NETMASK ADDRESS(255, 255, 255, 0)
#define GATEWAY ADDRESS(10, 0, 2, 2)
#define DNS_SERVER ADDRESS(10, 0, 2, 3)

#define HOSTS_FILE "/etc/hosts"
#define HOSTS_MAX 4096
#define ARP_MAX 8
#define CONN_MAX 8
#define RX_SIZE 32768
#define TX_SIZE 16384
#define MSS 1460
#define FRAME_MAX 1514
#define RTO_TICKS 100
#define RETRIES_MAX 8
#define CONNECT_TICKS 800
#define RECV_TICKS 3000
#define PING_TICKS 200
#define DNS_TICKS 300
#define ARP_TICKS 50
#define LINGER_TICKS 1000
#define CLOSE_TICKS 300
#define DNS_PORT_BASE 53000
#define EPHEMERAL_BASE 49152

typedef enum
{
    TCP_CLOSED,
    TCP_SYN_SENT,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK
} tcp_state_t;

typedef struct arp_entry
{
    uint32_t ip;
    uint8_t mac[6];
    int used;
} arp_entry_t;

typedef struct conn
{
    int used;
    int owner;
    tcp_state_t state;
    uint32_t remote_ip;
    uint16_t remote_port;
    uint16_t local_port;
    uint32_t iss;
    uint32_t snd_una;
    uint32_t snd_nxt;
    uint32_t rcv_nxt;
    uint32_t peer_window;
    int peer_closed;
    int error;
    uint64_t last_send;
    uint64_t closed_at;
    int retries;
    uint8_t rx[RX_SIZE];
    uint32_t rx_head;
    uint32_t rx_count;
    uint8_t tx[TX_SIZE];
    uint32_t tx_count;
} conn_t;

static spinlock_t net_lock = SPINLOCK_INIT;
static uint8_t local_mac[6];
static int up;
static arp_entry_t arp_table[ARP_MAX];
static conn_t conns[CONN_MAX];
static uint16_t next_port = EPHEMERAL_BASE;
static uint16_t ip_id = 1;
static uint64_t stats[4];
static char arp_channel;
static char ping_channel;
static char dns_channel;
static volatile uint32_t ping_reply_from;
static volatile uint16_t ping_reply_seq;
static volatile uint64_t ping_reply_tick;
static uint16_t dns_waiting_port;
static uint16_t dns_waiting_id;
static volatile uint32_t dns_answer;
static volatile int dns_done;
static uint8_t frame[FRAME_MAX];

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void copy(void *to, const void *from, uint32_t length)
{
    uint8_t *t = to;
    const uint8_t *f = from;

    for (uint32_t i = 0; i < length; i++)
    {
        t[i] = f[i];
    }
}

static uint32_t sum_words(const uint8_t *data, uint32_t length, uint32_t sum)
{
    for (uint32_t i = 0; i + 1 < length; i += 2)
    {
        sum += get16(data + i);
    }

    if (length & 1)
    {
        sum += (uint32_t)data[length - 1] << 8;
    }

    return sum;
}

static uint16_t fold(uint32_t sum)
{
    while (sum >> 16)
    {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return (uint16_t)~sum;
}

static uint16_t transport_checksum(uint32_t src, uint32_t dst, uint8_t protocol, const uint8_t *data,
                                   uint32_t length)
{
    uint32_t sum = (src >> 16) + (src & 0xFFFF) + (dst >> 16) + (dst & 0xFFFF) + protocol + length;

    return fold(sum_words(data, length, sum));
}

static void arp_learn(uint32_t ip, const uint8_t *mac)
{
    int slot = -1;

    for (int i = 0; i < ARP_MAX; i++)
    {
        if (arp_table[i].used && arp_table[i].ip == ip)
        {
            slot = i;
            break;
        }

        if (!arp_table[i].used && slot < 0)
        {
            slot = i;
        }
    }

    if (slot < 0)
    {
        slot = (int)(ip % ARP_MAX);
    }

    arp_table[slot].ip = ip;
    copy(arp_table[slot].mac, mac, 6);
    arp_table[slot].used = 1;
    process_wake(&arp_channel);
}

static int arp_lookup(uint32_t ip, uint8_t *mac)
{
    for (int i = 0; i < ARP_MAX; i++)
    {
        if (arp_table[i].used && arp_table[i].ip == ip)
        {
            copy(mac, arp_table[i].mac, 6);
            return 1;
        }
    }

    return 0;
}

static void send_frame(const uint8_t *mac, uint16_t type, uint32_t length)
{
    copy(frame, mac, 6);
    copy(frame + 6, local_mac, 6);
    put16(frame + 12, type);

    if (virtio_net_send(frame, 14 + length) == 0)
    {
        stats[1]++;
    }
}

static void arp_send(uint16_t op, const uint8_t *target_mac, uint32_t target_ip)
{
    static const uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t *a = frame + 14;

    put16(a, 1);
    put16(a + 2, ETH_IP);
    a[4] = 6;
    a[5] = 4;
    put16(a + 6, op);
    copy(a + 8, local_mac, 6);
    put32(a + 14, LOCAL_IP);
    copy(a + 18, op == 1 ? (const uint8_t *)"\0\0\0\0\0\0" : target_mac, 6);
    put32(a + 24, target_ip);
    send_frame(op == 1 ? broadcast : target_mac, ETH_ARP, 28);
}

static uint32_t next_hop(uint32_t ip)
{
    return (ip & NETMASK) == (LOCAL_IP & NETMASK) ? ip : GATEWAY;
}

static int resolve_mac(uint32_t ip, uint8_t *mac, int can_wait)
{
    uint32_t hop = next_hop(ip);

    if (arp_lookup(hop, mac))
    {
        return 0;
    }

    if (!can_wait)
    {
        arp_send(1, 0, hop);
        return -1;
    }

    for (int attempt = 0; attempt < 3; attempt++)
    {
        uint64_t interrupts = spin_lock(&net_lock);

        arp_send(1, 0, hop);
        spin_unlock(&net_lock, interrupts);

        if (!process_can_block())
        {
            return -1;
        }

        uint64_t enabled = irq_save();
        int found = arp_lookup(hop, mac);

        if (!found)
        {
            process_block(&arp_channel, ARP_TICKS);
            found = arp_lookup(hop, mac);
        }

        irq_restore(enabled);

        if (found)
        {
            return 0;
        }
    }

    return -1;
}

static int send_ip(uint32_t dst, uint8_t protocol, const uint8_t *payload, uint32_t length, int can_wait)
{
    uint8_t mac[6];

    if (length + 34 > FRAME_MAX || resolve_mac(dst, mac, can_wait) != 0)
    {
        return -1;
    }

    uint8_t *ip = frame + 14;

    ip[0] = 0x45;
    ip[1] = 0;
    put16(ip + 2, (uint16_t)(20 + length));
    put16(ip + 4, ip_id++);
    put16(ip + 6, 0x4000);
    ip[8] = 64;
    ip[9] = protocol;
    put16(ip + 10, 0);
    put32(ip + 12, LOCAL_IP);
    put32(ip + 16, dst);
    put16(ip + 10, fold(sum_words(ip, 20, 0)));
    copy(ip + 20, payload, length);
    send_frame(mac, ETH_IP, 20 + length);
    return 0;
}

static void tcp_send_segment(conn_t *c, uint8_t flags, uint32_t seq, const uint8_t *data, uint32_t length)
{
    static uint8_t segment[20 + 4 + MSS];
    uint32_t header = flags & TCP_SYN ? 24 : 20;
    uint32_t space = RX_SIZE - c->rx_count;

    put16(segment, c->local_port);
    put16(segment + 2, c->remote_port);
    put32(segment + 4, seq);
    put32(segment + 8, flags & TCP_ACK ? c->rcv_nxt : 0);
    put16(segment + 12, (uint16_t)(((header / 4) << 12) | flags));
    put16(segment + 14, (uint16_t)(space > 65535 ? 65535 : space));
    put16(segment + 16, 0);
    put16(segment + 18, 0);

    if (flags & TCP_SYN)
    {
        segment[20] = 2;
        segment[21] = 4;
        put16(segment + 22, MSS);
    }

    copy(segment + header, data, length);
    put16(segment + 16, transport_checksum(LOCAL_IP, c->remote_ip, IP_TCP, segment, header + length));
    send_ip(c->remote_ip, IP_TCP, segment, header + length, 0);
    c->last_send = timer_ticks();
}

static void tcp_ack(conn_t *c)
{
    tcp_send_segment(c, TCP_ACK, c->snd_nxt, 0, 0);
}

static void tcp_push(conn_t *c)
{
    if (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT)
    {
        return;
    }

    while (1)
    {
        uint32_t in_flight = c->snd_nxt - c->snd_una;
        uint32_t unsent = c->tx_count - in_flight;
        uint32_t window = c->peer_window > in_flight ? c->peer_window - in_flight : 0;
        uint32_t length = unsent < MSS ? unsent : MSS;

        if (length > window)
        {
            length = window;
        }

        if (length == 0)
        {
            return;
        }

        tcp_send_segment(c, TCP_ACK | TCP_PSH, c->snd_nxt, c->tx + in_flight, length);
        c->snd_nxt += length;
    }
}

static void conn_free(conn_t *c)
{
    c->used = 0;
    c->state = TCP_CLOSED;
    c->owner = 0;
}

static void tcp_input(uint32_t src, const uint8_t *seg, uint32_t length)
{
    if (length < 20)
    {
        return;
    }

    uint16_t sport = get16(seg);
    uint16_t dport = get16(seg + 2);
    uint32_t seq = get32(seg + 4);
    uint32_t ack = get32(seg + 8);
    uint32_t header = (uint32_t)(seg[12] >> 4) * 4;
    uint8_t flags = seg[13];
    uint32_t data_length = length > header ? length - header : 0;
    const uint8_t *data = seg + header;
    conn_t *c = 0;

    for (int i = 0; i < CONN_MAX; i++)
    {
        if (conns[i].used && conns[i].remote_ip == src && conns[i].remote_port == sport &&
            conns[i].local_port == dport)
        {
            c = &conns[i];
            break;
        }
    }

    if (!c || header < 20 || header > length)
    {
        return;
    }

    if (flags & TCP_RST)
    {
        c->error = c->state == TCP_SYN_SENT ? E_REFUSED : E_IO;
        c->peer_closed = 1;
        c->state = TCP_CLOSED;

        if (!c->owner)
        {
            conn_free(c);
        }

        process_wake(c);
        return;
    }

    c->peer_window = get16(seg + 14);

    if (c->state == TCP_SYN_SENT)
    {
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) && ack == c->iss + 1)
        {
            c->rcv_nxt = seq + 1;
            c->snd_una = ack;
            c->snd_nxt = ack;
            c->state = TCP_ESTABLISHED;
            c->retries = 0;
            tcp_ack(c);
            process_wake(c);
        }

        return;
    }

    if (flags & TCP_ACK)
    {
        uint32_t acked = ack - c->snd_una;
        uint32_t in_flight = c->snd_nxt - c->snd_una;

        if (acked > 0 && acked <= in_flight)
        {
            uint32_t data_acked = acked > c->tx_count ? c->tx_count : acked;

            for (uint32_t i = data_acked; i < c->tx_count; i++)
            {
                c->tx[i - data_acked] = c->tx[i];
            }

            c->tx_count -= data_acked;
            c->snd_una = ack;
            c->retries = 0;

            if (c->state == TCP_FIN_WAIT_1 && c->snd_una == c->snd_nxt)
            {
                c->state = TCP_FIN_WAIT_2;
            }
            else if (c->state == TCP_LAST_ACK && c->snd_una == c->snd_nxt)
            {
                conn_free(c);
                return;
            }

            process_wake(c);
        }
    }

    if (data_length > 0)
    {
        if (seq == c->rcv_nxt && data_length <= RX_SIZE - c->rx_count)
        {
            for (uint32_t i = 0; i < data_length; i++)
            {
                c->rx[(c->rx_head + c->rx_count + i) % RX_SIZE] = data[i];
            }

            c->rx_count += data_length;
            c->rcv_nxt += data_length;
            stats[2] += data_length;
            process_wake(c);
        }

        tcp_ack(c);
    }

    if ((flags & TCP_FIN) && seq + data_length == c->rcv_nxt)
    {
        c->rcv_nxt++;
        c->peer_closed = 1;
        tcp_ack(c);

        if (c->state == TCP_ESTABLISHED)
        {
            c->state = TCP_CLOSE_WAIT;
        }
        else if (c->state == TCP_FIN_WAIT_1 || c->state == TCP_FIN_WAIT_2)
        {
            conn_free(c);
            return;
        }

        process_wake(c);
    }

    tcp_push(c);
}

static uint32_t skip_name(const uint8_t *dns, uint32_t size, uint32_t p)
{
    while (p < size)
    {
        uint8_t length = dns[p];

        if (length == 0)
        {
            return p + 1;
        }

        if ((length & 0xC0) == 0xC0)
        {
            return p + 2;
        }

        p += (uint32_t)length + 1;
    }

    return size;
}

static void udp_input(uint32_t src, const uint8_t *udp, uint32_t length)
{
    if (length < 8 || src != DNS_SERVER || get16(udp) != 53 || get16(udp + 2) != dns_waiting_port)
    {
        return;
    }

    const uint8_t *dns = udp + 8;
    uint32_t size = length - 8;

    if (size < 12 || get16(dns) != dns_waiting_id)
    {
        return;
    }

    uint32_t questions = get16(dns + 4);
    uint32_t answers = get16(dns + 6);
    uint32_t p = 12;

    for (uint32_t q = 0; q < questions && p < size; q++)
    {
        p = skip_name(dns, size, p) + 4;
    }

    for (uint32_t a = 0; a < answers && p < size; a++)
    {
        p = skip_name(dns, size, p);

        if (p + 10 > size)
        {
            break;
        }

        uint16_t type = get16(dns + p);
        uint16_t data_length = get16(dns + p + 8);

        p += 10;

        if (type == 1 && data_length == 4 && p + 4 <= size)
        {
            dns_answer = get32(dns + p);
            break;
        }

        p += data_length;
    }

    dns_done = 1;
    process_wake(&dns_channel);
}

static void icmp_input(uint32_t src, const uint8_t *icmp, uint32_t length)
{
    static uint8_t reply[FRAME_MAX];

    if (length < 8 || length > sizeof(reply))
    {
        return;
    }

    if (icmp[0] == ICMP_ECHO_REQUEST)
    {
        copy(reply, icmp, length);
        reply[0] = ICMP_ECHO_REPLY;
        put16(reply + 2, 0);
        put16(reply + 2, fold(sum_words(reply, length, 0)));
        send_ip(src, IP_ICMP, reply, length, 0);
    }
    else if (icmp[0] == ICMP_ECHO_REPLY)
    {
        ping_reply_from = src;
        ping_reply_seq = get16(icmp + 6);
        ping_reply_tick = timer_ticks();
        process_wake(&ping_channel);
    }
}

void net_receive(const uint8_t *data, uint32_t length)
{
    if (length < 14 || !up)
    {
        return;
    }

    uint64_t interrupts = spin_lock(&net_lock);
    uint16_t type = get16(data + 12);
    const uint8_t *payload = data + 14;
    uint32_t size = length - 14;

    stats[0]++;

    if (type == ETH_ARP && size >= 28)
    {
        uint16_t op = get16(payload + 6);
        uint32_t sender = get32(payload + 14);
        uint32_t target = get32(payload + 24);

        arp_learn(sender, payload + 8);

        if (op == 1 && target == LOCAL_IP)
        {
            arp_send(2, payload + 8, sender);
        }
    }
    else if (type == ETH_IP && size >= 20 && (payload[0] >> 4) == 4)
    {
        uint32_t header = (uint32_t)(payload[0] & 0x0F) * 4;
        uint32_t total = get16(payload + 2);
        uint32_t src = get32(payload + 12);
        uint32_t dst = get32(payload + 16);

        if (header >= 20 && total <= size && total > header && dst == LOCAL_IP &&
            (get16(payload + 6) & 0x3FFF) == 0)
        {
            if (next_hop(src) == src)
            {
                arp_learn(src, data + 6);
            }

            const uint8_t *body = payload + header;
            uint32_t body_length = total - header;

            if (payload[9] == IP_TCP && transport_checksum(src, dst, IP_TCP, body, body_length) == 0)
            {
                tcp_input(src, body, body_length);
            }
            else if (payload[9] == IP_UDP)
            {
                udp_input(src, body, body_length);
            }
            else if (payload[9] == IP_ICMP && fold(sum_words(body, body_length, 0)) == 0)
            {
                icmp_input(src, body, body_length);
            }
        }
    }

    spin_unlock(&net_lock, interrupts);
}

void net_start(const uint8_t *mac)
{
    copy(local_mac, mac, 6);
    up = 1;
}

void net_tick(void)
{
    if (!up || spin_is_locked(&net_lock))
    {
        return;
    }

    uint64_t interrupts = spin_lock(&net_lock);
    uint64_t now = timer_ticks();

    for (int i = 0; i < CONN_MAX; i++)
    {
        conn_t *c = &conns[i];

        if (!c->used)
        {
            continue;
        }

        if (!c->owner && c->closed_at && now - c->closed_at > LINGER_TICKS)
        {
            conn_free(c);
            continue;
        }

        if (c->snd_una == c->snd_nxt || now - c->last_send < RTO_TICKS)
        {
            continue;
        }

        if (++c->retries > RETRIES_MAX)
        {
            c->error = E_TIMEOUT;
            c->state = TCP_CLOSED;
            process_wake(c);

            if (!c->owner)
            {
                conn_free(c);
            }

            continue;
        }

        if (c->state == TCP_SYN_SENT)
        {
            tcp_send_segment(c, TCP_SYN, c->iss, 0, 0);
        }
        else if (c->state == TCP_FIN_WAIT_1 || c->state == TCP_LAST_ACK)
        {
            if (c->tx_count == 0)
            {
                tcp_send_segment(c, TCP_FIN | TCP_ACK, c->snd_nxt - 1, 0, 0);
            }
        }
        else
        {
            c->snd_nxt = c->snd_una;
            tcp_push(c);
        }
    }

    spin_unlock(&net_lock, interrupts);
}

static conn_t *owned(int handle)
{
    if (handle < 1 || handle > CONN_MAX)
    {
        return 0;
    }

    conn_t *c = &conns[handle - 1];

    return c->used && c->owner == process_current_pid() ? c : 0;
}

static void close_locked(conn_t *c)
{
    c->owner = 0;
    c->closed_at = timer_ticks();

    if (c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT)
    {
        tcp_push(c);
        c->state = c->state == TCP_ESTABLISHED ? TCP_FIN_WAIT_1 : TCP_LAST_ACK;
        tcp_send_segment(c, TCP_FIN | TCP_ACK, c->snd_nxt, 0, 0);
        c->snd_nxt++;
    }
    else if (c->state == TCP_CLOSED || c->state == TCP_SYN_SENT)
    {
        conn_free(c);
    }
}

void net_release(int pid)
{
    uint64_t interrupts = spin_lock(&net_lock);

    for (int i = 0; i < CONN_MAX; i++)
    {
        if (conns[i].used && conns[i].owner == pid)
        {
            close_locked(&conns[i]);
        }
    }

    spin_unlock(&net_lock, interrupts);
}

void net_info(net_info_t *info)
{
    uint64_t interrupts = spin_lock(&net_lock);

    info->up = (uint32_t)up;
    info->address = LOCAL_IP;
    info->netmask = NETMASK;
    info->gateway = GATEWAY;
    info->dns = DNS_SERVER;
    copy(info->mac, local_mac, 6);
    info->frames_in = stats[0];
    info->frames_out = stats[1];
    info->bytes_in = stats[2];
    info->bytes_out = stats[3];
    info->connections = 0;

    for (int i = 0; i < CONN_MAX; i++)
    {
        info->connections += conns[i].used ? 1 : 0;
    }

    spin_unlock(&net_lock, interrupts);
}

static int parse_ip(const char *text, uint32_t *address)
{
    uint32_t value = 0;
    int parts = 0;
    int digits = 0;
    uint32_t part = 0;

    for (int i = 0;; i++)
    {
        char ch = text[i];

        if (ch >= '0' && ch <= '9')
        {
            part = part * 10 + (uint32_t)(ch - '0');
            digits++;

            if (part > 255 || digits > 3)
            {
                return -1;
            }
        }
        else if ((ch == '.' || ch == 0) && digits > 0)
        {
            value = (value << 8) | part;
            parts++;
            part = 0;
            digits = 0;

            if (ch == 0)
            {
                break;
            }
        }
        else
        {
            return -1;
        }
    }

    if (parts != 4)
    {
        return -1;
    }

    *address = value;
    return 0;
}

static char lower(char ch)
{
    return ch >= 'A' && ch <= 'Z' ? (char)(ch - 'A' + 'a') : ch;
}

static int is_space(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r';
}

static int hosts_search(char *hosts, const char *name, uint32_t *address)
{

    for (char *line = hosts; *line;)
    {
        char *end = line;

        while (*end && *end != '\n')
        {
            end++;
        }

        char *next = *end ? end + 1 : end;
        char *hash = line;

        while (hash < end && *hash != '#')
        {
            hash++;
        }

        *hash = 0;

        char *p = line;
        char ip[16];
        uint32_t n = 0;

        while (is_space(*p))
        {
            p++;
        }

        while (*p && !is_space(*p) && n < sizeof(ip) - 1)
        {
            ip[n++] = *p++;
        }

        ip[n] = 0;

        uint32_t value;

        if (n > 0 && parse_ip(ip, &value) == 0)
        {
            while (*p)
            {
                while (is_space(*p))
                {
                    p++;
                }

                uint32_t i = 0;

                while (p[i] && !is_space(p[i]) && name[i] && lower(p[i]) == lower(name[i]))
                {
                    i++;
                }

                if (i > 0 && name[i] == 0 && (p[i] == 0 || is_space(p[i])))
                {
                    *address = value;
                    return 0;
                }

                while (*p && !is_space(*p))
                {
                    p++;
                }
            }
        }

        line = next;
    }

    return -1;
}

static int hosts_lookup(const char *name, uint32_t *address)
{
    uint32_t inode;

    if (knocfs_lookup(HOSTS_FILE, &inode) != 0)
    {
        return -1;
    }

    char *hosts = kmalloc(HOSTS_MAX + 1);

    if (!hosts)
    {
        return -1;
    }

    int64_t length = knocfs_read(inode, 0, hosts, HOSTS_MAX);
    int result = -1;

    if (length > 0)
    {
        hosts[length] = 0;
        result = hosts_search(hosts, name, address);
    }

    kfree(hosts);
    return result;
}

int64_t net_resolve(const char *name, uint32_t *address)
{
    static uint8_t query[300];
    static uint16_t sequence;

    if (parse_ip(name, address) == 0 || hosts_lookup(name, address) == 0)
    {
        return 0;
    }

    if (!up)
    {
        return E_NETDOWN;
    }

    uint32_t p = 12;
    uint32_t n = 0;

    for (int i = 0;; i++)
    {
        char ch = name[i];

        if (ch == '.' || ch == 0)
        {
            if (n == 0 || n > 63 || p + n + 1 > 250)
            {
                return E_INVAL;
            }

            query[p] = (uint8_t)n;
            p += n + 1;
            n = 0;

            if (ch == 0)
            {
                break;
            }

            continue;
        }

        if (p + 1 + n < 250)
        {
            query[p + 1 + n] = (uint8_t)ch;
        }

        n++;
    }

    query[p++] = 0;
    put16(query + p, 1);
    put16(query + p + 2, 1);
    p += 4;

    for (int attempt = 0; attempt < 2; attempt++)
    {
        uint64_t interrupts = spin_lock(&net_lock);
        uint16_t port = (uint16_t)(DNS_PORT_BASE + (++sequence % 1000));
        static uint8_t packet[8 + 300];

        dns_waiting_port = port;
        dns_waiting_id = (uint16_t)(0x4B00 + sequence);
        dns_done = 0;
        dns_answer = 0;
        put16(query, dns_waiting_id);
        put16(query + 2, 0x0100);
        put16(query + 4, 1);
        put16(query + 6, 0);
        put16(query + 8, 0);
        put16(query + 10, 0);
        put16(packet, port);
        put16(packet + 2, 53);
        put16(packet + 4, (uint16_t)(8 + p));
        put16(packet + 6, 0);
        copy(packet + 8, query, p);
        put16(packet + 6, transport_checksum(LOCAL_IP, DNS_SERVER, IP_UDP, packet, 8 + p));
        spin_unlock(&net_lock, interrupts);

        uint8_t mac[6];

        if (resolve_mac(DNS_SERVER, mac, 1) != 0)
        {
            return E_NETDOWN;
        }

        interrupts = spin_lock(&net_lock);
        send_ip(DNS_SERVER, IP_UDP, packet, 8 + p, 0);
        spin_unlock(&net_lock, interrupts);

        uint64_t deadline = timer_ticks() + DNS_TICKS;
        uint64_t enabled = irq_save();

        while (!dns_done && timer_ticks() < deadline)
        {
            process_block(&dns_channel, deadline - timer_ticks());
        }

        irq_restore(enabled);

        if (dns_done)
        {
            dns_waiting_port = 0;

            if (!dns_answer)
            {
                return E_NOTFOUND;
            }

            *address = dns_answer;
            return 0;
        }
    }

    dns_waiting_port = 0;
    return E_TIMEOUT;
}

int64_t net_ping(uint32_t address, uint32_t sequence)
{
    uint8_t packet[64];
    uint8_t mac[6];

    if (!up)
    {
        return E_NETDOWN;
    }

    if (resolve_mac(address, mac, 1) != 0)
    {
        return E_TIMEOUT;
    }

    for (uint32_t i = 0; i < sizeof(packet); i++)
    {
        packet[i] = (uint8_t)i;
    }

    packet[0] = ICMP_ECHO_REQUEST;
    packet[1] = 0;
    put16(packet + 2, 0);
    put16(packet + 4, (uint16_t)process_current_pid());
    put16(packet + 6, (uint16_t)sequence);
    put16(packet + 2, fold(sum_words(packet, sizeof(packet), 0)));

    uint64_t interrupts = spin_lock(&net_lock);

    ping_reply_from = 0;
    send_ip(address, IP_ICMP, packet, sizeof(packet), 0);

    uint64_t sent = timer_ticks();

    spin_unlock(&net_lock, interrupts);

    uint64_t deadline = sent + PING_TICKS;
    uint64_t enabled = irq_save();

    while (timer_ticks() < deadline)
    {
        if (ping_reply_from == address && ping_reply_seq == (uint16_t)sequence)
        {
            irq_restore(enabled);
            return (int64_t)((ping_reply_tick - sent) * 10);
        }

        process_block(&ping_channel, deadline - timer_ticks());
    }

    irq_restore(enabled);
    return E_TIMEOUT;
}

int64_t net_connect(uint32_t address, uint32_t port)
{
    uint8_t mac[6];

    if (!up)
    {
        return E_NETDOWN;
    }

    if (port == 0 || port > 65535)
    {
        return E_INVAL;
    }

    if (resolve_mac(address, mac, 1) != 0)
    {
        return E_TIMEOUT;
    }

    uint64_t interrupts = spin_lock(&net_lock);
    conn_t *c = 0;
    int handle = 0;

    for (int i = 0; i < CONN_MAX; i++)
    {
        if (!conns[i].used)
        {
            c = &conns[i];
            handle = i + 1;
            break;
        }
    }

    if (!c)
    {
        spin_unlock(&net_lock, interrupts);
        return E_NOMEM;
    }

    c->used = 1;
    c->owner = process_current_pid();
    c->state = TCP_SYN_SENT;
    c->remote_ip = address;
    c->remote_port = (uint16_t)port;
    c->local_port = next_port;
    next_port = next_port >= 65000 ? EPHEMERAL_BASE : (uint16_t)(next_port + 1);
    c->iss = (uint32_t)(timer_read() * 2654435761UL);
    c->snd_una = c->iss;
    c->snd_nxt = c->iss + 1;
    c->rcv_nxt = 0;
    c->peer_window = MSS;
    c->peer_closed = 0;
    c->error = 0;
    c->closed_at = 0;
    c->retries = 0;
    c->rx_head = 0;
    c->rx_count = 0;
    c->tx_count = 0;
    tcp_send_segment(c, TCP_SYN, c->iss, 0, 0);
    spin_unlock(&net_lock, interrupts);

    uint64_t deadline = timer_ticks() + CONNECT_TICKS;
    uint64_t enabled = irq_save();

    while (c->state == TCP_SYN_SENT && !c->error && timer_ticks() < deadline)
    {
        process_block(c, deadline - timer_ticks());
    }

    irq_restore(enabled);

    interrupts = spin_lock(&net_lock);

    int64_t result = c->state == TCP_ESTABLISHED ? handle : c->error ? c->error : E_TIMEOUT;

    if (result < 0)
    {
        conn_free(c);
    }

    spin_unlock(&net_lock, interrupts);
    return result;
}

int64_t net_send(int handle, const uint8_t *data, uint64_t length)
{
    uint64_t sent = 0;

    while (sent < length)
    {
        uint64_t interrupts = spin_lock(&net_lock);
        conn_t *c = owned(handle);

        if (!c)
        {
            spin_unlock(&net_lock, interrupts);
            return E_BADF;
        }

        if (c->error || (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT))
        {
            int64_t error = c->error ? c->error : E_IO;

            spin_unlock(&net_lock, interrupts);
            return sent ? (int64_t)sent : error;
        }

        uint32_t space = TX_SIZE - c->tx_count;
        uint64_t chunk = length - sent < space ? length - sent : space;

        copy(c->tx + c->tx_count, data + sent, (uint32_t)chunk);
        c->tx_count += (uint32_t)chunk;
        sent += chunk;
        stats[3] += chunk;
        tcp_push(c);
        spin_unlock(&net_lock, interrupts);

        if (sent < length)
        {
            uint64_t enabled = irq_save();

            if (c->tx_count == TX_SIZE && !c->error)
            {
                process_block(c, RTO_TICKS);
            }

            irq_restore(enabled);
        }
    }

    return (int64_t)sent;
}

int64_t net_recv(int handle, uint8_t *data, uint64_t length)
{
    uint64_t deadline = timer_ticks() + RECV_TICKS;

    while (1)
    {
        uint64_t interrupts = spin_lock(&net_lock);
        conn_t *c = owned(handle);

        if (!c)
        {
            spin_unlock(&net_lock, interrupts);
            return E_BADF;
        }

        if (c->rx_count > 0)
        {
            uint32_t was_full = c->rx_count > RX_SIZE / 2;
            uint64_t n = length < c->rx_count ? length : c->rx_count;

            for (uint64_t i = 0; i < n; i++)
            {
                data[i] = c->rx[(c->rx_head + i) % RX_SIZE];
            }

            c->rx_head = (c->rx_head + (uint32_t)n) % RX_SIZE;
            c->rx_count -= (uint32_t)n;

            if (was_full)
            {
                tcp_ack(c);
            }

            spin_unlock(&net_lock, interrupts);
            return (int64_t)n;
        }

        if (c->peer_closed || c->error || c->state == TCP_CLOSED)
        {
            int64_t result = c->error && !c->peer_closed ? c->error : 0;

            spin_unlock(&net_lock, interrupts);
            return result;
        }

        spin_unlock(&net_lock, interrupts);

        if (timer_ticks() >= deadline)
        {
            return E_TIMEOUT;
        }

        uint64_t enabled = irq_save();

        if (c->rx_count == 0 && !c->peer_closed && !c->error && c->state != TCP_CLOSED)
        {
            process_block(c, deadline - timer_ticks());
        }

        irq_restore(enabled);
    }
}

int64_t net_close(int handle)
{
    uint64_t deadline = timer_ticks() + CLOSE_TICKS;
    conn_t *c = owned(handle);

    if (!c)
    {
        return E_BADF;
    }

    uint64_t enabled = irq_save();

    while (c->tx_count > 0 && !c->error && c->state != TCP_CLOSED && timer_ticks() < deadline)
    {
        process_block(c, deadline - timer_ticks());
    }

    irq_restore(enabled);

    uint64_t interrupts = spin_lock(&net_lock);

    close_locked(c);
    spin_unlock(&net_lock, interrupts);
    return 0;
}
