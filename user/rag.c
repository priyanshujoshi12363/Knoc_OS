#include "ulib.h"
#include "rag.h"
#include "embed.h"

#define WORDS_MAX 24
#define WORD_LENGTH 64
#define NODES_PER_WORD 3
#define ENTITY_NODES_MAX 4
#define LINKS_PER_NODE 3
#define LINKS_SCAN 12
#define RECENT_SCAN 300
#define SEEN_MAX 64
#define HEALTH_EDGES_MAX 8
#define MEANING_CANDIDATES 64
#define MEANING_NODES 2
#define MEANING_MIN 0.5f

typedef struct builder
{
    char *text;
    int length;
} builder_t;

static char words[WORDS_MAX][WORD_LENGTH];
static int word_count;
static unsigned int seen_links[SEEN_MAX];
static graph_edge_info_t health_edges[HEALTH_EDGES_MAX];
static graph_edge_info_t problem_edges[HEALTH_EDGES_MAX];
static int seen_count;

static const char *stop_words[] = {
    "the", "and", "for", "why", "what", "who", "how", "was", "were", "did", "does", "are", "you",
    "your", "this", "that", "with", "from", "have", "has", "can", "there", "about", "into", "when",
    "where", "which", "file", "files", "program", "programs", "happened", "happen", "tell", "please",
    "its", "it's", "any", "all", "been", "doing", "done", "now", "right", "get", "got", "not", "but",
    "then", "than", "them", "they", "will", "would", "could", "should", "some", "just", "more", "most",
    "much", "many", "is", "my", "me", "do", "to", "of", "in", "on", "it", "a", "an", "or", "if",
};

static const char *health_words[] = {
    "slow", "wrong", "problem", "problems", "issue", "issues", "health", "healthy", "leak", "leaking",
    "hog", "storm", "lag", "laggy", "anomaly", "anomalies", "fix", "fixed", "stuck", "freeze", "frozen",
    "hang", "healthd", "stopped", "killed", "priority",
};

static const char *resource_words[] = {
    "ram", "memory", "cpu", "disk", "space", "running", "processes", "load", "usage", "storage",
};

static const char *here_words[] = {
    "my", "this", "now", "current", "currently", "free", "used", "left", "much", "full", "right", "here",
};

static const char *crash_words[] = {
    "crash", "crashed", "crashes", "crashing", "fail", "failed", "failure", "error", "errors", "broke",
    "broken", "fault", "panic", "driver", "drivers", "recover", "recovered", "recovery", "restart",
    "restarted", "safe", "guardian", "reboot",
};

static const char *context_words[] = {
    "working", "work", "project", "projects", "context", "doing", "did", "recently", "recent", "yesterday",
    "today", "busy", "focus", "continue",
};

static const char *file_words[] = {
    "moved", "move", "organize", "organized", "organizer", "download", "downloads", "folder", "sorted",
    "sort", "classified",
};

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

static int word_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' ||
           c == '.' || c == '_' || c == '-' || c == '\'' || (unsigned char)c >= 0x80;
}

static void split_words(const char *question)
{
    word_count = 0;

    for (int i = 0; question[i] && word_count < WORDS_MAX;)
    {
        while (question[i] && !word_char(question[i]))
        {
            i++;
        }

        int length = 0;

        while (question[i] && word_char(question[i]))
        {
            if (length < WORD_LENGTH - 1)
            {
                words[word_count][length++] = lower(question[i]);
            }

            i++;
        }

        while (length > 0 && (words[word_count][length - 1] == '.' || words[word_count][length - 1] == '\''))
        {
            length--;
        }

        if (length > 0)
        {
            words[word_count][length] = 0;
            word_count++;
        }
    }
}

static int in_list(const char *word, const char **list, unsigned long count)
{
    for (unsigned long i = 0; i < count; i++)
    {
        if (strcmp(word, list[i]) == 0)
        {
            return 1;
        }
    }

    return 0;
}

#define IN(word, list) in_list(word, list, sizeof(list) / sizeof(list[0]))

static int asks_about(const char **list, unsigned long count)
{
    for (int i = 0; i < word_count; i++)
    {
        if (in_list(words[i], list, count))
        {
            return 1;
        }
    }

    return 0;
}

#define ASKS(list) asks_about(list, sizeof(list) / sizeof(list[0]))

static void put(builder_t *b, const char *text)
{
    while (*text && b->length < RAG_FACT_LENGTH - 1)
    {
        b->text[b->length++] = *text++;
    }

    b->text[b->length] = 0;
}

static void put_uint(builder_t *b, unsigned long value)
{
    char digits[24];
    int n = 0;

    do
    {
        digits[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value && n < 23);

    char text[24];

    for (int i = 0; i < n; i++)
    {
        text[i] = digits[n - 1 - i];
    }

    text[n] = 0;
    put(b, text);
}

static const char *base_name(const char *path)
{
    const char *base = path;

    for (int i = 0; path[i]; i++)
    {
        if (path[i] == '/' && path[i + 1])
        {
            base = path + i + 1;
        }
    }

    return base;
}

static void put_folder(builder_t *b, const char *path)
{
    const char *base = base_name(path);

    if (base == path)
    {
        put(b, "/");
        return;
    }

    while (path < base - 1 && b->length < RAG_FACT_LENGTH - 1)
    {
        b->text[b->length++] = *path++;
    }

    b->text[b->length] = 0;
}

static builder_t start_fact(rag_facts_t *facts)
{
    builder_t b = {0, 0};

    if (facts->count < RAG_FACTS_MAX)
    {
        b.text = facts->text[facts->count];
        b.text[0] = 0;
    }

    return b;
}

static void finish_fact(rag_facts_t *facts, builder_t *b)
{
    if (b->text == 0 || b->length == 0)
    {
        return;
    }

    for (int i = 0; i < facts->count; i++)
    {
        if (strcmp(facts->text[i], b->text) == 0)
        {
            return;
        }
    }

    facts->count++;
}

static int seen_before(unsigned int seq)
{
    for (int i = 0; i < seen_count; i++)
    {
        if (seen_links[i] == seq)
        {
            return 1;
        }
    }

    return 0;
}

static int already_seen(unsigned int seq)
{
    if (seen_before(seq))
    {
        return 1;
    }

    if (seen_count < SEEN_MAX)
    {
        seen_links[seen_count++] = seq;
    }

    return 0;
}

static void describe_link(rag_facts_t *facts, const graph_edge_info_t *e)
{
    if (facts->count >= RAG_FACTS_MAX || already_seen(e->seq))
    {
        return;
    }

    builder_t b = start_fact(facts);

    switch (e->relation)
    {
    case GRAPH_REL_CLASSIFIED_AS:
        put(&b, "The file ");
        put(&b, e->from);
        put(&b, " is a ");
        put(&b, e->to);
        put(&b, " file (");
        put_uint(&b, e->confidence);
        put(&b, "% sure).");
        break;
    case GRAPH_REL_CAME_FROM:
        if (strcmp(e->to, "other") == 0)
        {
            return;
        }

        put(&b, "The file ");
        put(&b, e->from);
        put(&b, " came from ");
        put(&b, e->to);
        put(&b, ".");
        break;
    case GRAPH_REL_MOVED_TO:
        put(&b, e->actor);
        put(&b, " moved ");
        put(&b, base_name(e->from));
        put(&b, " from ");
        put_folder(&b, e->from);
        put(&b, " to ");
        put_folder(&b, e->to);
        put(&b, ".");
        break;
    case GRAPH_REL_RESTORED_TO:
        put(&b, e->actor);
        put(&b, " moved ");
        put(&b, e->from);
        put(&b, " back to ");
        put(&b, e->to);
        put(&b, ".");
        break;
    case GRAPH_REL_STARTED:
        put(&b, e->from);
        put(&b, " started the program ");
        put(&b, e->to);
        put(&b, ".");
        break;
    case GRAPH_REL_CRASHED:
        put(&b, "The program ");
        put(&b, e->from);
        put(&b, " crashed: ");
        put(&b, e->to);
        break;
    case GRAPH_REL_CRASHED_IN:
        put(&b, e->from);
        put(&b, " crashed inside the driver ");
        put(&b, e->to);
        put(&b, ".");
        break;
    case GRAPH_REL_DIAGNOSED_AS:
        put(&b, "The AI space diagnosed ");
        put(&b, e->from);
        put(&b, " as: ");
        put(&b, e->to);
        break;
    case GRAPH_REL_ACTION:
        put(&b, "For ");
        put(&b, e->from);
        put(&b, " the AI space decided to ");
        put(&b, e->to);
        put(&b, ".");
        break;
    case GRAPH_REL_DISABLED:
        put(&b, e->from);
        put(&b, " disabled the driver ");
        put(&b, e->to);
        put(&b, " because it kept crashing.");
        break;
    case GRAPH_REL_DENIED:
        put(&b, "The program ");
        put(&b, e->from);
        put(&b, " was refused the ");
        put(&b, e->to);
        put(&b, " permission.");
        break;
    case GRAPH_REL_RESTARTED:
        put(&b, e->from);
        put(&b, " restarted the program ");
        put(&b, e->to);
        put(&b, " after it crashed.");
        break;
    case GRAPH_REL_STOPPED:
        put(&b, e->from);
        put(&b, strcmp(e->from, "ai-space") == 0 ? " stopped the crashing program " : " stopped the program ");
        put(&b, e->to);
        put(&b, strcmp(e->from, "healthd") == 0 ? " to fix a problem it caused." : ".");
        break;
    case GRAPH_REL_IN_PROCESS:
        put(&b, e->from);
        put(&b, " happened in the program ");
        put(&b, e->to);
        put(&b, ".");
        break;
    case GRAPH_REL_ANOMALY:
        put(&b, "healthd found a ");
        put(&b, e->to);
        put(&b, " caused by the program ");
        put(&b, e->from);
        put(&b, ".");
        break;
    case GRAPH_REL_LOWERED:
        put(&b, e->from);
        put(&b, " moved the program ");
        put(&b, e->to);
        put(&b, " to background priority so it stops slowing the system.");
        break;
    default:
        return;
    }

    finish_fact(facts, &b);
}

static void request_init(graph_request_t *request, unsigned int op, unsigned long index)
{
    memset(request, 0, sizeof(*request));
    request->op = op;
    request->index = (unsigned int)index;
}

static void copy_text(char *to, const char *from, unsigned long room)
{
    unsigned long i = 0;

    while (from[i] && i < room - 1)
    {
        to[i] = from[i];
        i++;
    }

    to[i] = 0;
}

static int equal_ignoring_case(const char *a, const char *b)
{
    while (*a && lower(*a) == lower(*b))
    {
        a++;
        b++;
    }

    return *a == 0 && *b == 0;
}

static int useful_kind(unsigned int kind)
{
    return kind == GRAPH_KIND_FILE || kind == GRAPH_KIND_FOLDER || kind == GRAPH_KIND_PROGRAM ||
           kind == GRAPH_KIND_DRIVER || kind == GRAPH_KIND_CRASH || kind == GRAPH_KIND_SOURCE;
}

static graph_edge_info_t node_edges[LINKS_SCAN];

static const graph_edge_info_t *find_edge(int count, unsigned int relation, const char *from)
{
    for (int i = 0; i < count; i++)
    {
        if (node_edges[i].relation == relation && strcmp(node_edges[i].from, from) == 0)
        {
            return &node_edges[i];
        }
    }

    return 0;
}

static int explain_move(rag_facts_t *facts, int count)
{
    const graph_edge_info_t *moved = find_edge(count, GRAPH_REL_MOVED_TO, node_edges[0].from);

    for (int i = 0; !moved && i < count; i++)
    {
        if (node_edges[i].relation == GRAPH_REL_MOVED_TO)
        {
            moved = &node_edges[i];
        }
    }

    const graph_edge_info_t *type = moved ? find_edge(count, GRAPH_REL_CLASSIFIED_AS, moved->from) : 0;

    if (!type || facts->count >= RAG_FACTS_MAX || seen_before(moved->seq) || seen_before(type->seq))
    {
        return 0;
    }

    already_seen(moved->seq);
    already_seen(type->seq);

    builder_t b = start_fact(facts);

    put(&b, moved->actor);
    put(&b, " moved ");
    put(&b, base_name(moved->from));
    put(&b, " from ");
    put_folder(&b, moved->from);
    put(&b, " to ");
    put_folder(&b, moved->to);
    put(&b, " because it is a ");
    put(&b, type->to);
    put(&b, " file (");
    put_uint(&b, type->confidence);
    put(&b, "% sure).");
    finish_fact(facts, &b);
    return 1;
}

static void node_links(rag_facts_t *facts, const graph_node_info_t *node)
{
    graph_request_t request;
    int count = 0;

    for (unsigned long i = 0; i < LINKS_SCAN; i++)
    {
        request_init(&request, GRAPH_OP_EDGES, i);
        request.kind_a = node->kind;
        copy_text(request.a, node->name, sizeof(request.a));

        if (graph(&request, &node_edges[count]) != 0)
        {
            break;
        }

        if (node_edges[count].relation != GRAPH_REL_STARTED)
        {
            count++;
        }
    }

    int added = explain_move(facts, count);

    for (int i = 0; i < count && added < LINKS_PER_NODE && facts->count < RAG_FACTS_MAX; i++)
    {
        int before = facts->count;

        describe_link(facts, &node_edges[i]);
        added += facts->count > before;
    }
}

static embed_model_t embed_model;
static int embed_state;
static graph_node_info_t candidates[MEANING_CANDIDATES];

static int candidate_known(int count, unsigned int kind, const char *name)
{
    for (int i = 0; i < count; i++)
    {
        if (candidates[i].kind == kind && strcmp(candidates[i].name, name) == 0)
        {
            return 1;
        }
    }

    return 0;
}

static void meaning_facts(rag_facts_t *facts, const char *question)
{
    graph_request_t request;
    graph_edge_info_t edge;
    int count = 0;

    if (embed_state == 0)
    {
        embed_state = embed_load(&embed_model, EMBED_MODEL) == 0 ? 1 : -1;
    }

    if (embed_state < 0)
    {
        return;
    }

    for (unsigned long i = 0; i < RECENT_SCAN && count < MEANING_CANDIDATES; i++)
    {
        request_init(&request, GRAPH_OP_RECENT, i);

        if (graph(&request, &edge) != 0)
        {
            break;
        }

        if (useful_kind(edge.from_kind) && !candidate_known(count, edge.from_kind, edge.from) &&
            count < MEANING_CANDIDATES)
        {
            memset(&candidates[count], 0, sizeof(candidates[count]));
            candidates[count].kind = edge.from_kind;
            copy_text(candidates[count].name, edge.from, sizeof(candidates[count].name));
            count++;
        }

        if (useful_kind(edge.to_kind) && !candidate_known(count, edge.to_kind, edge.to) && count < MEANING_CANDIDATES)
        {
            memset(&candidates[count], 0, sizeof(candidates[count]));
            candidates[count].kind = edge.to_kind;
            copy_text(candidates[count].name, edge.to, sizeof(candidates[count].name));
            count++;
        }
    }

    float q[EMBED_DIM_MAX];
    float v[EMBED_DIM_MAX];
    float best_score[MEANING_NODES] = {0};
    int best[MEANING_NODES] = {-1, -1};

    embed_text(&embed_model, question, strlen(question), q);

    for (int i = 0; i < count; i++)
    {
        embed_text(&embed_model, candidates[i].name, strlen(candidates[i].name), v);

        float score = embed_dot(q, v, embed_model.dim);

        for (int slot = 0; slot < MEANING_NODES; slot++)
        {
            if (score >= MEANING_MIN && (best[slot] < 0 || score > best_score[slot]))
            {
                for (int move = MEANING_NODES - 1; move > slot; move--)
                {
                    best[move] = best[move - 1];
                    best_score[move] = best_score[move - 1];
                }

                best[slot] = i;
                best_score[slot] = score;
                break;
            }
        }
    }

    for (int slot = 0; slot < MEANING_NODES && best[slot] >= 0 && facts->count < RAG_FACTS_MAX; slot++)
    {
        node_links(facts, &candidates[best[slot]]);
    }
}

static void entity_facts(rag_facts_t *facts)
{
    graph_request_t request;
    graph_node_info_t node;
    int used_nodes = 0;

    for (int w = 0; w < word_count && used_nodes < ENTITY_NODES_MAX; w++)
    {
        const char *word = words[w];

        if (strlen(word) < 3 || IN(word, stop_words))
        {
            continue;
        }

        int taken = 0;

        for (unsigned long i = 0; i < 64 && taken < NODES_PER_WORD && used_nodes < ENTITY_NODES_MAX; i++)
        {
            request_init(&request, GRAPH_OP_FIND, i);
            copy_text(request.a, word, sizeof(request.a));

            if (graph(&request, &node) != 0)
            {
                break;
            }

            int exact = equal_ignoring_case(node.name, word) || equal_ignoring_case(base_name(node.name), word);
            int path_like = word[0] == '/' || strlen(word) >= 6;

            if (!useful_kind(node.kind) || (!exact && !path_like))
            {
                continue;
            }

            node_links(facts, &node);
            taken++;
            used_nodes++;
        }
    }
}

static const char *problem_phrase(const char *diagnosis)
{
    if (strcmp(diagnosis, "memory leak") == 0)
    {
        return " was leaking memory";
    }

    if (strcmp(diagnosis, "CPU hog") == 0)
    {
        return " was hogging the CPU";
    }

    if (strcmp(diagnosis, "disk thrashing") == 0)
    {
        return " was thrashing the disk";
    }

    if (strcmp(diagnosis, "spawn storm") == 0)
    {
        return " was starting too many programs";
    }

    if (strcmp(diagnosis, "repeated permission denials") == 0)
    {
        return " kept asking for things it has no permission for";
    }

    return " was filling up the disk";
}

static void problem_fact(rag_facts_t *facts, const graph_edge_info_t *problem, int fixes)
{
    const graph_edge_info_t *fix = 0;

    for (int i = 0; i < fixes; i++)
    {
        if (strcmp(health_edges[i].to, problem->from) == 0 && health_edges[i].seq > problem->seq)
        {
            fix = &health_edges[i];
        }
    }

    if (facts->count >= RAG_FACTS_MAX || already_seen(problem->seq))
    {
        return;
    }

    builder_t b = start_fact(facts);

    put(&b, "The program ");
    put(&b, problem->from);
    put(&b, problem_phrase(problem->to));

    if (fix)
    {
        put(&b, fix->relation == GRAPH_REL_STOPPED ? "; healthd fixed it by stopping " :
                                                     "; healthd fixed it by moving it to background priority");
        put(&b, fix->relation == GRAPH_REL_STOPPED ? problem->from : "");
    }

    put(&b, ".");
    finish_fact(facts, &b);
}

static void recent_facts(rag_facts_t *facts, int want_health, int want_crash, int want_files)
{
    graph_request_t request;
    graph_edge_info_t edge;
    int problems = 0;
    int fixes = 0;
    int crash = 0;
    int files = 0;

    for (unsigned long i = 0; i < RECENT_SCAN && facts->count < RAG_FACTS_MAX; i++)
    {
        request_init(&request, GRAPH_OP_RECENT, i);

        if (graph(&request, &edge) != 0)
        {
            break;
        }

        int from_healthd = strcmp(edge.actor, "healthd") == 0;
        int is_fix = from_healthd && (edge.relation == GRAPH_REL_STOPPED || edge.relation == GRAPH_REL_LOWERED);
        int is_crash = !from_healthd &&
                       (edge.relation == GRAPH_REL_CRASHED || edge.relation == GRAPH_REL_DISABLED ||
                        edge.relation == GRAPH_REL_RESTARTED || edge.relation == GRAPH_REL_STOPPED ||
                        edge.relation == GRAPH_REL_CRASHED_IN);
        int is_file = edge.relation == GRAPH_REL_MOVED_TO || edge.relation == GRAPH_REL_RESTORED_TO;

        if (want_health && is_fix && fixes < HEALTH_EDGES_MAX)
        {
            health_edges[fixes++] = edge;
        }
        else if (want_health && edge.relation == GRAPH_REL_ANOMALY && problems < HEALTH_EDGES_MAX)
        {
            problem_edges[problems++] = edge;
        }
        else if (want_crash && is_crash && crash < 3)
        {
            describe_link(facts, &edge);
            crash++;
        }
        else if (want_files && is_file && files < 3)
        {
            describe_link(facts, &edge);
            files++;
        }
    }

    for (int i = 0; i < problems && i < 4; i++)
    {
        problem_fact(facts, &problem_edges[i], fixes);
    }
}

static void now_fact(rag_facts_t *facts)
{
    telemetry_sample_t s;

    if (facts->count >= RAG_FACTS_MAX || telemetry(0, &s) != 0)
    {
        return;
    }

    builder_t b = start_fact(facts);

    put(&b, "Right now: CPU ");
    put_uint(&b, s.cpu_busy);
    put(&b, "% busy, RAM ");
    put_uint(&b, (s.ram_total_kib - s.ram_free_kib) / 1024);
    put(&b, " of ");
    put_uint(&b, s.ram_total_kib / 1024);
    put(&b, " MiB used, disk ");
    put_uint(&b, s.disk_free_kib / 1024);
    put(&b, " of ");
    put_uint(&b, s.disk_total_kib / 1024);
    put(&b, " MiB free, ");
    put_uint(&b, s.processes);
    put(&b, " processes.");

    if (s.top_cpu_name[0] && s.top_cpu > 20 && strcmp(s.top_cpu_name, "ask") != 0)
    {
        put(&b, " The busiest program is ");
        put(&b, s.top_cpu_name);
        put(&b, " (");
        put_uint(&b, s.top_cpu);
        put(&b, "% CPU).");
    }

    finish_fact(facts, &b);
}

static void crash_facts(rag_facts_t *facts)
{
    system_info_t info;
    crash_info_t crash;

    if (facts->count < RAG_FACTS_MAX && sysinfo(&info) == 0)
    {
        builder_t b = start_fact(facts);

        put(&b, "The AI space is ");
        put(&b, info.ai_online ? "online" : "offline");
        put(&b, "; it recorded ");
        put_uint(&b, info.crashes_total);
        put(&b, " kernel crashes and restarted the kernel ");
        put_uint(&b, info.kernel_restarts);
        put(&b, " times");
        put(&b, info.safe_mode ? "; the system is in safe mode." : ".");

        if (info.disabled_drivers[0][0])
        {
            put(&b, " Disabled driver: ");
            put(&b, info.disabled_drivers[0]);
            put(&b, ".");
        }

        finish_fact(facts, &b);
    }

    for (unsigned long i = 0; i < 2 && facts->count < RAG_FACTS_MAX && crashinfo(i, &crash) == 0; i++)
    {
        builder_t b = start_fact(facts);

        put(&b, "Kernel crash #");
        put_uint(&b, crash.sequence);
        put(&b, " in ");
        put(&b, crash.process[0] ? crash.process : "the kernel");
        put(&b, ": ");
        put(&b, crash.message);
        put(&b, ". Diagnosis: ");
        put(&b, crash.diagnosis);
        finish_fact(facts, &b);
    }
}

#define CONTEXT_ITEMS 16

typedef struct tally
{
    char name[GRAPH_NAME_MAX];
    unsigned int count;
} tally_t;

static tally_t folder_tally[CONTEXT_ITEMS];
static tally_t program_tally[CONTEXT_ITEMS];

static void tally_add(tally_t *items, const char *name, unsigned int weight)
{
    int free_slot = -1;

    for (int i = 0; i < CONTEXT_ITEMS; i++)
    {
        if (items[i].count && strcmp(items[i].name, name) == 0)
        {
            items[i].count += weight;
            return;
        }

        if (!items[i].count && free_slot < 0)
        {
            free_slot = i;
        }
    }

    if (free_slot >= 0)
    {
        copy_text(items[free_slot].name, name, GRAPH_NAME_MAX);
        items[free_slot].count = weight;
    }
}

static void folder_part(const char *path, char *out)
{
    unsigned long length = strlen(path);

    while (length > 1 && path[length - 1] != '/')
    {
        length--;
    }

    length = length > 1 ? length - 1 : 1;
    memcpy(out, path, length);
    out[length] = 0;
}

static int tally_top(tally_t *items, builder_t *b, int limit)
{
    int shown = 0;

    for (int n = 0; n < limit; n++)
    {
        int best = -1;

        for (int i = 0; i < CONTEXT_ITEMS; i++)
        {
            if (items[i].count && (best < 0 || items[i].count > items[best].count))
            {
                best = i;
            }
        }

        if (best < 0)
        {
            break;
        }

        put(b, shown ? ", " : " ");
        put(b, items[best].name);
        items[best].count = 0;
        shown++;
    }

    return shown;
}

static void context_facts(rag_facts_t *facts)
{
    graph_request_t request;
    graph_edge_info_t edge;
    char folder[PATH_MAX];

    memset(folder_tally, 0, sizeof(folder_tally));
    memset(program_tally, 0, sizeof(program_tally));

    for (unsigned long i = 0; i < RECENT_SCAN * 2; i++)
    {
        request_init(&request, GRAPH_OP_RECENT, i);

        if (graph(&request, &edge) != 0)
        {
            break;
        }

        if (edge.relation == GRAPH_REL_WORKED_IN)
        {
            if (edge.to_kind == GRAPH_KIND_FOLDER)
            {
                tally_add(folder_tally, edge.to, 3);
            }
            else
            {
                folder_part(edge.to, folder);
                tally_add(folder_tally, folder, 1);
            }
        }
        else if (edge.relation == GRAPH_REL_MOVED_TO && strcmp(edge.actor, "organize") != 0)
        {
            folder_part(edge.to, folder);
            tally_add(folder_tally, folder, 1);
        }
        else if (edge.relation == GRAPH_REL_STARTED && strcmp(edge.from, "knocsh") == 0 &&
                 strcmp(edge.to, "knocsh") != 0 && strcmp(edge.to, "healthd") != 0 &&
                 strcmp(edge.to, "organized") != 0)
        {
            tally_add(program_tally, edge.to, 1);
        }
    }

    if (facts->count < RAG_FACTS_MAX)
    {
        builder_t b = start_fact(facts);

        put(&b, "Recently the user worked most in:");

        if (tally_top(folder_tally, &b, 3))
        {
            put(&b, ".");
            finish_fact(facts, &b);
        }
    }

    if (facts->count < RAG_FACTS_MAX)
    {
        builder_t b = start_fact(facts);

        put(&b, "Programs the user ran recently:");

        if (tally_top(program_tally, &b, 4))
        {
            put(&b, ".");
            finish_fact(facts, &b);
        }
    }
}

void rag_collect(const char *question, rag_facts_t *facts)
{
    facts->count = 0;
    seen_count = 0;
    split_words(question);

    int want_health = ASKS(health_words) || (ASKS(resource_words) && ASKS(here_words));
    int want_crash = ASKS(crash_words);
    int want_files = ASKS(file_words);

    entity_facts(facts);

    if (facts->count == 0 && !want_health && !want_crash)
    {
        meaning_facts(facts, question);
    }

    if (facts->count > 0)
    {
        want_files = 0;
    }

    if (want_health)
    {
        now_fact(facts);
    }

    if (want_crash)
    {
        crash_facts(facts);
    }

    if (want_health || want_crash || want_files)
    {
        recent_facts(facts, want_health, want_crash, want_files);
    }

    if (ASKS(context_words))
    {
        context_facts(facts);
    }
}
