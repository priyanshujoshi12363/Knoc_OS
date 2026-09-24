#include "memgraph.h"
#include "knocfs.h"
#include "page.h"
#include "process.h"
#include "string.h"
#include "timer.h"

#define GRAPH_MAGIC 0x3148504152474B4EULL
#define GRAPH_VERSION 1
#define NODE_CAPACITY 4096
#define EDGE_CAPACITY 16384
#define HASH_SLOTS 8192
#define NO_NODE 0xFFFFFFFFU

#define HEADER_PATH "/memory/header"
#define NODES_PATH "/memory/nodes"
#define EDGES_PATH "/memory/edges"

typedef struct graph_header
{
    uint64_t magic;
    uint32_t version;
    uint32_t node_count;
    uint32_t edge_count;
    uint32_t edge_next;
    uint32_t seq;
    uint32_t reserved;
} graph_header_t;

typedef struct graph_node
{
    uint32_t kind;
    uint32_t hits;
    uint32_t first_boot;
    uint32_t last_boot;
    uint64_t last_uptime;
    uint32_t used;
    uint32_t reserved;
    char name[GRAPH_NAME_MAX];
} graph_node_t;

typedef struct graph_edge
{
    uint32_t from;
    uint32_t to;
    uint32_t actor;
    uint16_t relation;
    uint8_t confidence;
    uint8_t used;
    uint32_t boot;
    uint32_t seq;
    uint64_t uptime;
} graph_edge_t;

_Static_assert(sizeof(graph_node_t) == 128, "graph nodes are 128 bytes");
_Static_assert(sizeof(graph_edge_t) == 32, "graph edges are 32 bytes");

static graph_header_t header;
static graph_node_t *nodes;
static graph_edge_t *edges;
static uint32_t *hash;
static uint32_t header_inode;
static uint32_t nodes_inode;
static uint32_t edges_inode;
static uint32_t current_boot;
static int ready;
static sleeplock_t graph_lock = SLEEPLOCK_INIT;

static void copy_name(char *to, const char *from)
{
    int i = 0;

    while (from[i] && i < GRAPH_NAME_MAX - 1)
    {
        to[i] = from[i];
        i++;
    }

    while (i < GRAPH_NAME_MAX)
    {
        to[i++] = 0;
    }
}

static int same_name(const char *a, const char *b)
{
    for (int i = 0; i < GRAPH_NAME_MAX; i++)
    {
        if (a[i] != b[i])
        {
            return 0;
        }

        if (a[i] == 0)
        {
            return 1;
        }
    }

    return 1;
}

static uint32_t key_hash(uint32_t kind, const char *name)
{
    uint32_t value = 0x811C9DC5u ^ kind;

    for (int i = 0; i < GRAPH_NAME_MAX - 1 && name[i]; i++)
    {
        value ^= (uint8_t)name[i];
        value *= 0x01000193u;
    }

    return value % HASH_SLOTS;
}

static void hash_insert(uint32_t id)
{
    uint32_t slot = key_hash(nodes[id].kind, nodes[id].name);

    while (hash[slot] != NO_NODE)
    {
        slot = (slot + 1) % HASH_SLOTS;
    }

    hash[slot] = id;
}

static void hash_rebuild(void)
{
    for (uint32_t i = 0; i < HASH_SLOTS; i++)
    {
        hash[i] = NO_NODE;
    }

    for (uint32_t id = 0; id < NODE_CAPACITY; id++)
    {
        if (nodes[id].used)
        {
            hash_insert(id);
        }
    }
}

static uint32_t lookup(uint32_t kind, const char *name)
{
    char key[GRAPH_NAME_MAX];
    uint32_t slot;

    copy_name(key, name);
    slot = key_hash(kind, key);

    while (hash[slot] != NO_NODE)
    {
        graph_node_t *node = &nodes[hash[slot]];

        if (node->used && node->kind == kind && same_name(node->name, key))
        {
            return hash[slot];
        }

        slot = (slot + 1) % HASH_SLOTS;
    }

    return NO_NODE;
}

static int open_file(const char *path, uint32_t *inode)
{
    int result = knocfs_lookup(path, inode);

    if (result == E_NOTFOUND)
    {
        result = knocfs_create(path, KNOCFS_TYPE_FILE, inode);
    }

    return result;
}

static void save_header(void)
{
    knocfs_write(header_inode, 0, &header, sizeof(header));
}

static void save_node(uint32_t id)
{
    knocfs_write(nodes_inode, (uint64_t)id * sizeof(graph_node_t), &nodes[id], sizeof(graph_node_t));
}

static void save_edge(uint32_t index)
{
    knocfs_write(edges_inode, (uint64_t)index * sizeof(graph_edge_t), &edges[index], sizeof(graph_edge_t));
}

int memgraph_init(uint32_t boot)
{
    uint32_t folder;

    ready = 0;
    current_boot = boot;

    if (!knocfs_mounted())
    {
        return -1;
    }

    if (knocfs_lookup("/memory", &folder) == E_NOTFOUND &&
        knocfs_create("/memory", KNOCFS_TYPE_DIR, &folder) != 0)
    {
        return -1;
    }

    if (open_file(HEADER_PATH, &header_inode) != 0 ||
        open_file(NODES_PATH, &nodes_inode) != 0 ||
        open_file(EDGES_PATH, &edges_inode) != 0)
    {
        return -1;
    }

    if (nodes == 0)
    {
        nodes = page_alloc_contiguous(NODE_CAPACITY * sizeof(graph_node_t));
        edges = page_alloc_contiguous(EDGE_CAPACITY * sizeof(graph_edge_t));
        hash = page_alloc_contiguous(HASH_SLOTS * sizeof(uint32_t));

        if (nodes == 0 || edges == 0 || hash == 0)
        {
            return -1;
        }
    }

    memset(nodes, 0, NODE_CAPACITY * sizeof(graph_node_t));
    memset(edges, 0, EDGE_CAPACITY * sizeof(graph_edge_t));

    if (knocfs_read(header_inode, 0, &header, sizeof(header)) != sizeof(header) ||
        header.magic != GRAPH_MAGIC || header.version != GRAPH_VERSION)
    {
        memset(&header, 0, sizeof(header));
        header.magic = GRAPH_MAGIC;
        header.version = GRAPH_VERSION;
        knocfs_truncate(nodes_inode);
        knocfs_truncate(edges_inode);
        save_header();
    }
    else
    {
        knocfs_read(nodes_inode, 0, nodes, NODE_CAPACITY * sizeof(graph_node_t));
        knocfs_read(edges_inode, 0, edges, EDGE_CAPACITY * sizeof(graph_edge_t));
    }

    hash_rebuild();
    ready = 1;
    return 0;
}

int memgraph_ready(void)
{
    return ready;
}

static uint32_t node_for(uint32_t kind, const char *name, int *created)
{
    uint32_t id = lookup(kind, name);

    *created = 0;

    if (id != NO_NODE)
    {
        return id;
    }

    for (id = 0; id < NODE_CAPACITY; id++)
    {
        if (!nodes[id].used)
        {
            break;
        }
    }

    if (id == NODE_CAPACITY)
    {
        return NO_NODE;
    }

    memset(&nodes[id], 0, sizeof(graph_node_t));
    nodes[id].used = 1;
    nodes[id].kind = kind;
    nodes[id].first_boot = current_boot;
    copy_name(nodes[id].name, name);
    hash_insert(id);
    header.node_count++;
    *created = 1;
    return id;
}

static void touch(uint32_t id)
{
    nodes[id].hits++;
    nodes[id].last_boot = current_boot;
    nodes[id].last_uptime = timer_ticks();
    save_node(id);
}

int memgraph_record(uint32_t kind_a, const char *a, uint32_t relation,
                    uint32_t kind_b, const char *b, const char *actor, uint32_t confidence)
{
    int created;

    if (!ready || kind_a == 0 || kind_a >= GRAPH_KIND_COUNT || kind_b == 0 ||
        kind_b >= GRAPH_KIND_COUNT || relation == 0 || relation >= GRAPH_REL_COUNT ||
        a[0] == 0 || b[0] == 0)
    {
        return E_INVAL;
    }

    sleeplock_acquire(&graph_lock);

    uint32_t from = node_for(kind_a, a, &created);
    uint32_t to = node_for(kind_b, b, &created);
    uint32_t who = node_for(GRAPH_KIND_ACTOR, actor, &created);

    if (from == NO_NODE || to == NO_NODE || who == NO_NODE)
    {
        sleeplock_release(&graph_lock);
        return E_NOSPACE;
    }

    touch(from);
    touch(to);
    touch(who);

    uint32_t index = EDGE_CAPACITY;

    for (uint32_t i = 0; i < EDGE_CAPACITY; i++)
    {
        if (edges[i].used && edges[i].from == from && edges[i].to == to &&
            edges[i].relation == relation)
        {
            index = i;
            break;
        }
    }

    if (index == EDGE_CAPACITY)
    {
        index = header.edge_next;
        header.edge_next = (header.edge_next + 1) % EDGE_CAPACITY;

        if (!edges[index].used)
        {
            header.edge_count++;
        }
    }

    graph_edge_t *edge = &edges[index];

    edge->used = 1;
    edge->from = from;
    edge->to = to;
    edge->actor = who;
    edge->relation = (uint16_t)relation;
    edge->confidence = (uint8_t)(confidence > 100 ? 100 : confidence);
    edge->boot = current_boot;
    edge->uptime = timer_ticks();
    edge->seq = ++header.seq;

    save_edge(index);
    save_header();

    sleeplock_release(&graph_lock);
    return 0;
}

int memgraph_stats(graph_stats_t *stats)
{
    memset(stats, 0, sizeof(*stats));

    if (!ready)
    {
        return E_IO;
    }

    sleeplock_acquire(&graph_lock);

    for (uint32_t id = 0; id < NODE_CAPACITY; id++)
    {
        if (nodes[id].used)
        {
            stats->nodes++;

            if (nodes[id].kind < GRAPH_KIND_COUNT)
            {
                stats->by_kind[nodes[id].kind]++;
            }
        }
    }

    for (uint32_t i = 0; i < EDGE_CAPACITY; i++)
    {
        stats->edges += edges[i].used;
    }

    stats->node_capacity = NODE_CAPACITY;
    stats->edge_capacity = EDGE_CAPACITY;
    stats->boot = current_boot;

    sleeplock_release(&graph_lock);
    return 0;
}

static void edge_info(const graph_edge_t *edge, graph_edge_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->seq = edge->seq;
    info->boot = edge->boot;
    info->uptime = edge->uptime;
    info->relation = edge->relation;
    info->confidence = edge->confidence;
    info->from_kind = nodes[edge->from].kind;
    info->to_kind = nodes[edge->to].kind;
    copy_name(info->from, nodes[edge->from].name);
    copy_name(info->to, nodes[edge->to].name);
    copy_name(info->actor, nodes[edge->actor].name);
}

static int nth_newest(uint32_t node, uint32_t index, graph_edge_info_t *info)
{
    uint32_t last_seq = 0xFFFFFFFFU;

    for (uint32_t n = 0; n <= index; n++)
    {
        uint32_t best = EDGE_CAPACITY;

        for (uint32_t i = 0; i < EDGE_CAPACITY; i++)
        {
            graph_edge_t *edge = &edges[i];

            if (!edge->used || edge->seq >= last_seq)
            {
                continue;
            }

            if (node != NO_NODE && edge->from != node && edge->to != node)
            {
                continue;
            }

            if (best == EDGE_CAPACITY || edge->seq > edges[best].seq)
            {
                best = i;
            }
        }

        if (best == EDGE_CAPACITY)
        {
            return E_NOTFOUND;
        }

        last_seq = edges[best].seq;

        if (n == index)
        {
            edge_info(&edges[best], info);
        }
    }

    return 0;
}

int memgraph_recent(uint32_t index, graph_edge_info_t *info)
{
    if (!ready)
    {
        return E_IO;
    }

    sleeplock_acquire(&graph_lock);
    int result = nth_newest(NO_NODE, index, info);
    sleeplock_release(&graph_lock);
    return result;
}

int memgraph_edges(uint32_t kind, const char *name, uint32_t index, graph_edge_info_t *info)
{
    if (!ready)
    {
        return E_IO;
    }

    sleeplock_acquire(&graph_lock);

    uint32_t node = lookup(kind, name);
    int result = node == NO_NODE ? E_NOTFOUND : nth_newest(node, index, info);

    sleeplock_release(&graph_lock);
    return result;
}

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

static int contains(const char *text, const char *part)
{
    if (part[0] == 0)
    {
        return 1;
    }

    for (int i = 0; text[i]; i++)
    {
        int j = 0;

        while (part[j] && lower(text[i + j]) == lower(part[j]))
        {
            j++;
        }

        if (part[j] == 0)
        {
            return 1;
        }
    }

    return 0;
}

int memgraph_find(const char *text, uint32_t index, graph_node_info_t *info)
{
    if (!ready)
    {
        return E_IO;
    }

    sleeplock_acquire(&graph_lock);

    uint32_t seen = 0;
    int result = E_NOTFOUND;

    for (uint32_t id = 0; id < NODE_CAPACITY; id++)
    {
        graph_node_t *node = &nodes[id];

        if (!node->used || !contains(node->name, text) || seen++ != index)
        {
            continue;
        }

        memset(info, 0, sizeof(*info));
        info->kind = node->kind;
        info->hits = node->hits;
        info->first_boot = node->first_boot;
        info->last_boot = node->last_boot;
        copy_name(info->name, node->name);

        for (uint32_t i = 0; i < EDGE_CAPACITY; i++)
        {
            info->edges += edges[i].used && (edges[i].from == id || edges[i].to == id);
        }

        result = 0;
        break;
    }

    sleeplock_release(&graph_lock);
    return result;
}

int memgraph_forget(uint32_t kind, const char *name)
{
    if (!ready)
    {
        return E_IO;
    }

    sleeplock_acquire(&graph_lock);

    uint32_t id = lookup(kind, name);

    if (id == NO_NODE)
    {
        sleeplock_release(&graph_lock);
        return E_NOTFOUND;
    }

    for (uint32_t i = 0; i < EDGE_CAPACITY; i++)
    {
        if (edges[i].used && (edges[i].from == id || edges[i].to == id))
        {
            edges[i].used = 0;
            header.edge_count--;
            save_edge(i);
        }
    }

    memset(&nodes[id], 0, sizeof(graph_node_t));
    header.node_count--;
    save_node(id);
    save_header();
    hash_rebuild();

    sleeplock_release(&graph_lock);
    return 0;
}
