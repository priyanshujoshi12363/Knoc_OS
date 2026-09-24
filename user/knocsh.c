#include "ulib.h"

/* knocsh: the KnocOS shell. Reads a line, runs a built-in command or a
   program from /bin, and waits for it. Everything goes through system
   calls: the shell is an ordinary user program. */

#define LINE_MAX 128
#define WORDS_MAX 8
#define PARTS_MAX 32
#define CAT_CHUNK 256
#define MIB (1024UL * 1024)

static char cwd[PATH_MAX] = "/";
static char line[LINE_MAX];

static const char *class_names[] = {"INTERACTIVE", "AI_AGENT", "NORMAL", "BACKGROUND", "IDLE"};
static const char *state_names[] = {"UNUSED", "READY", "RUNNING", "SLEEPING", "EXITED", "CRASHED", "LOADING", "BLOCKED"};
static const char *crash_types[] = {"NONE", "PANIC", "TRAP", "FREEZE"};
static const char *action_names[] = {"none", "reboot", "reboot into safe mode", "halt",
                                     "warm kernel restart", "warm kernel restart into safe mode"};

static void print_size(unsigned long value)
{
    char digits[21];
    int count = 0;

    do
    {
        digits[count++] = '0' + (value % 10);
        value /= 10;
    } while (value > 0);

    for (int i = count; i < 10; i++)
    {
        print(" ");
    }

    while (count > 0)
    {
        write(FD_STDOUT, &digits[--count], 1);
    }
}

static const char *error_text(long code)
{
    switch (code)
    {
    case E_NOTFOUND:
        return "not found";
    case E_PERM:
        return "permission denied";
    case E_EXISTS:
        return "already exists";
    case E_NOTEMPTY:
        return "directory not empty";
    case E_ISDIR:
        return "is a directory";
    case E_NOTDIR:
        return "not a directory";
    case E_NOSPACE:
        return "no space left";
    case E_FAULT:
        return "bad address";
    case E_INVAL:
        return "invalid argument";
    case E_IO:
        return "disk error";
    default:
        return "failed";
    }
}

static void fail(const char *what, long code)
{
    print("knocsh: ");
    print(what);
    print(": ");
    print(error_text(code));
    print("\n");
}

/* ---- Input ---- */

static void read_line(void)
{
    unsigned long length = 0;

    while (1)
    {
        char c;

        if (read(FD_STDIN, &c, 1) <= 0)
        {
            continue;
        }

        if (c == '\r' || c == '\n')
        {
            print("\n");
            line[length] = 0;
            return;
        }

        if (c == 0x7F || c == 0x08)
        {
            if (length > 0)
            {
                length--;
                print("\b \b");
            }

            continue;
        }

        if (c < ' ' || length >= LINE_MAX - 1)
        {
            continue;
        }

        line[length++] = c;
        write(FD_STDOUT, &c, 1);
    }
}

static int split(char *text, char **args)
{
    int count = 0;

    while (*text && count < WORDS_MAX)
    {
        while (*text == ' ')
        {
            *text++ = 0;
        }

        if (*text == 0)
        {
            break;
        }

        args[count++] = text;

        while (*text && *text != ' ')
        {
            text++;
        }
    }

    return count;
}

/* ---- Paths: relative to the current directory, with . and .. ---- */

static void resolve(const char *path, char *out)
{
    char joined[PATH_MAX * 2];
    char *parts[PARTS_MAX];
    int count = 0;
    unsigned long length = 0;

    if (path[0] != '/')
    {
        for (unsigned long i = 0; cwd[i] && length < sizeof(joined) - 2; i++)
        {
            joined[length++] = cwd[i];
        }

        joined[length++] = '/';
    }

    for (unsigned long i = 0; path[i] && length < sizeof(joined) - 1; i++)
    {
        joined[length++] = path[i];
    }

    joined[length] = 0;

    char *part = joined;

    while (*part)
    {
        while (*part == '/')
        {
            *part++ = 0;
        }

        if (*part == 0)
        {
            break;
        }

        char *start = part;

        while (*part && *part != '/')
        {
            part++;
        }

        if (*part == '/')
        {
            *part++ = 0;
        }

        if (strcmp(start, ".") == 0)
        {
            continue;
        }

        if (strcmp(start, "..") == 0)
        {
            count -= count > 0;
            continue;
        }

        if (count < PARTS_MAX)
        {
            parts[count++] = start;
        }
    }

    length = 0;

    for (int i = 0; i < count; i++)
    {
        out[length++] = '/';

        for (unsigned long j = 0; parts[i][j] && length < PATH_MAX - 1; j++)
        {
            out[length++] = parts[i][j];
        }
    }

    if (length == 0)
    {
        out[length++] = '/';
    }

    out[length] = 0;
}

/* ---- Files ---- */

static void cmd_ls(int argc, char **args)
{
    char path[PATH_MAX];
    file_stat_t info;
    dir_entry_t entry;

    resolve(argc > 1 ? args[1] : ".", path);

    long result = stat(path, &info);

    if (result != 0)
    {
        fail(path, result);
        return;
    }

    if (info.type != FILE_TYPE_DIR)
    {
        print_size(info.size);
        print("  ");
        print(path);
        print("\n");
        return;
    }

    for (unsigned long i = 0; readdir(path, i, &entry) == 0; i++)
    {
        if (entry.type == FILE_TYPE_DIR)
        {
            print_padded("     <dir>", 10);
            print("  ");
            print(entry.name);
            print("/\n");
        }
        else
        {
            print_size(entry.size);
            print("  ");
            print(entry.name);
            print("\n");
        }
    }
}

static void cmd_cd(int argc, char **args)
{
    char path[PATH_MAX];
    file_stat_t info;

    resolve(argc > 1 ? args[1] : "/", path);

    long result = stat(path, &info);

    if (result != 0)
    {
        fail(path, result);
    }
    else if (info.type != FILE_TYPE_DIR)
    {
        fail(path, E_NOTDIR);
    }
    else
    {
        strcpy(cwd, path);
    }
}

static void cmd_cat(int argc, char **args)
{
    char path[PATH_MAX];
    char buffer[CAT_CHUNK];

    if (argc < 2)
    {
        print("usage: cat FILE\n");
        return;
    }

    resolve(args[1], path);

    file_stat_t info;
    long result = stat(path, &info);

    if (result == 0 && info.type == FILE_TYPE_DIR)
    {
        result = E_ISDIR;
    }

    int fd = result == 0 ? open(path, O_READ) : (int)result;

    if (fd < 0)
    {
        fail(path, fd);
        return;
    }

    long count;
    int newline = 1;

    while ((count = read(fd, buffer, sizeof(buffer))) > 0)
    {
        write(FD_STDOUT, buffer, (unsigned long)count);
        newline = buffer[count - 1] == '\n';
    }

    close(fd);

    if (!newline)
    {
        print("\n");
    }
}

/* echo WORDS... [> FILE] */
static void cmd_echo(int argc, char **args)
{
    char text[LINE_MAX];
    unsigned long length = 0;
    int redirect = 0;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(args[i], ">") == 0)
        {
            redirect = i;
            break;
        }

        if (length > 0)
        {
            text[length++] = ' ';
        }

        for (unsigned long j = 0; args[i][j] && length < sizeof(text) - 2; j++)
        {
            text[length++] = args[i][j];
        }
    }

    text[length++] = '\n';

    if (!redirect)
    {
        write(FD_STDOUT, text, length);
        return;
    }

    if (redirect + 1 >= argc)
    {
        print("usage: echo TEXT > FILE\n");
        return;
    }

    char path[PATH_MAX];

    resolve(args[redirect + 1], path);

    int fd = open(path, O_WRITE | O_CREATE | O_TRUNC);

    if (fd < 0)
    {
        fail(path, fd);
        return;
    }

    write(fd, text, length);
    close(fd);
}

static void cmd_path_change(int argc, char **args, int make)
{
    char path[PATH_MAX];

    if (argc < 2)
    {
        print(make ? "usage: mkdir DIR\n" : "usage: rm PATH\n");
        return;
    }

    resolve(args[1], path);

    long result = make ? mkdir(path) : remove(path);

    if (result != 0)
    {
        fail(path, result);
    }
}

/* ---- Processes ---- */

static void cmd_ps(void)
{
    process_info_t info;

    print("  PID  NAME          CLASS        STATE     CPU  MEMORY    MODE\n");

    for (unsigned long i = 0; ps(i, &info) == 0; i++)
    {
        print("  ");
        print_padded_uint((unsigned long)info.pid, 5);
        print_padded(info.name, 14);
        print_padded(info.process_class < 5 ? class_names[info.process_class] : "?", 13);
        print_padded(info.state < 8 ? state_names[info.state] : "?", 10);
        print_padded_uint(info.cpu_ticks, 5);
        print_padded_uint(info.memory / 1024, 6);
        print_padded("KiB", 4);
        print(info.user ? "user" : "kernel");
        print("\n");
    }
}

static void cmd_kill(int argc, char **args)
{
    if (argc < 2)
    {
        print("usage: kill PID | NAME\n");
        return;
    }

    long pid = parse_number(args[1]);

    if (pid >= 0)
    {
        long result = kill((int)pid);

        if (result != 0)
        {
            fail(result == E_PERM ? "kill (only user programs can be stopped)" : "kill", result);
        }

        return;
    }

    process_info_t info;
    int pids[32];
    int count = 0;

    for (unsigned long i = 0; ps(i, &info) == 0 && count < 32; i++)
    {
        if (info.user && strcmp(info.name, args[1]) == 0 && strcmp(info.name, "knocsh") != 0)
        {
            pids[count++] = info.pid;
        }
    }

    for (int i = 0; i < count; i++)
    {
        kill(pids[i]);
    }

    if (count == 0)
    {
        fail(args[1], E_NOTFOUND);
    }
}

static int program_exists(const char *name)
{
    char path[PATH_MAX] = "/bin/";
    file_stat_t info;

    if (strlen(name) >= PATH_MAX - 6)
    {
        return 0;
    }

    strcpy(path + 5, name);
    return stat(path, &info) == 0 && info.type == FILE_TYPE_FILE;
}

static void join_args(int argc, char **args, int first, char *out)
{
    unsigned long length = 0;

    out[0] = 0;

    for (int i = first; i < argc; i++)
    {
        char resolved[PATH_MAX];
        const char *word = args[i];
        file_stat_t info;

        if (strcmp(word, "&") == 0)
        {
            continue;
        }

        if (word[0] != '-' && word[0] != '/')
        {
            resolve(word, resolved);

            if (stat(resolved, &info) == 0)
            {
                word = resolved;
            }
        }

        if (length > 0 && length < ARGS_MAX - 1)
        {
            out[length++] = ' ';
        }

        for (unsigned long j = 0; word[j] && length < ARGS_MAX - 1; j++)
        {
            out[length++] = word[j];
        }

        out[length] = 0;
    }
}

static void run_program(const char *name, int background, int argc, char **args, int first)
{
    char program_args[ARGS_MAX];

    join_args(argc, args, first, program_args);

    int pid = spawn_args(name, program_args);

    if (pid < 0)
    {
        fail(name, pid);
        return;
    }

    if (background)
    {
        print("[");
        print_uint((unsigned long)pid);
        print("] ");
        print(name);
        print(" started in the background\n");
        return;
    }

    long code = wait(pid);

    if (code == E_KILLED)
    {
        print("knocsh: ");
        print(name);
        print(" stopped (Ctrl-C)\n");
    }
    else if (code == E_CRASHED)
    {
        print("knocsh: ");
        print(name);
        print(" crashed; the AI space is handling it\n");
    }
    else if (code != 0)
    {
        print("knocsh: ");
        print(name);
        print(" exited with code ");
        print_uint((unsigned long)code);
        print("\n");
    }
}

/* ---- System ---- */

static void print_mib(unsigned long bytes)
{
    print_uint(bytes / MIB);
    print(" MiB");
}

static void cmd_mem(void)
{
    system_info_t info;

    if (sysinfo(&info) != 0)
    {
        fail("mem", E_PERM);
        return;
    }

    print("RAM:  ");
    print_mib(info.ram_bytes);
    print(" total, ");
    print_mib(info.ram_free_bytes);
    print(" free\n");

    print("Disk: ");
    print_mib(info.disk_bytes);
    print(" total, ");
    print_mib(info.disk_free_bytes);
    print(" free, ");
    print_uint(info.disk_files);
    print(" files\n");
}

static void cmd_devices(void)
{
    device_info_t info;

    for (unsigned long i = 0; devinfo(i, &info) == 0; i++)
    {
        print("  ");
        print_padded(info.name, 10);

        if (info.irq != 0)
        {
            print("IRQ ");
            print_padded_uint(info.irq, 4);
        }
        else
        {
            print_padded("", 8);
        }

        if (info.blocks != 0)
        {
            print_padded_uint(info.blocks, 9);
            print("sectors  ");
        }
        else
        {
            print_padded("", 18);
        }

        print(info.ready ? "ready" : info.disabled ? "disabled by the AI" : "failed");
        print("\n");
    }
}

static void cmd_crashes(void)
{
    crash_info_t info;
    unsigned long shown = 0;

    for (unsigned long i = 0; crashinfo(i, &info) == 0; i++)
    {
        print("#");
        print_uint(info.sequence);
        print(" ");
        print(info.crash_type < 4 ? crash_types[info.crash_type] : "?");
        print(" in ");
        print(info.process);
        print(" (pid ");
        print_uint((unsigned long)info.pid);
        print(") after ");
        print_uint(info.uptime_ticks / 100);
        print(" s: ");
        print(info.message);
        print("\n");

        if (info.driver[0])
        {
            print("   inside driver: ");
            print(info.driver);
            print("\n");
        }

        print("   AI diagnosis: ");
        print(info.diagnosis);
        print("\n   AI action: ");
        print(info.action < 6 ? action_names[info.action] : "?");
        print("\n");
        shown++;
    }

    if (shown == 0)
    {
        print("No crashes recorded in the black box\n");
    }
}

static void cmd_ai(void)
{
    system_info_t info;

    if (sysinfo(&info) != 0)
    {
        fail("ai", E_PERM);
        return;
    }

    if (!info.ai_online)
    {
        print("AI space: offline (run QEMU with -smp 2)\n");
        return;
    }

    print("AI space: online on core 1, running for ");
    print_uint(info.ai_uptime_ms / 1000);
    print(" s\n");
    print("Warm kernel restarts: ");
    print_uint(info.kernel_restarts);
    print("\nCrashes in the black box: ");
    print_uint(info.crashes_total);
    print(" (");
    print_uint(info.crashes_in_a_row);
    print(" in a row)\nSafe mode: ");
    print(info.safe_mode ? "on" : "off");
    print("\nDrivers disabled by the AI:");

    int any = 0;

    for (int i = 0; i < INFO_DISABLED_MAX; i++)
    {
        if (info.disabled_drivers[i][0])
        {
            print(" ");
            print(info.disabled_drivers[i]);
            any = 1;
        }
    }

    print(any ? "\n" : " none\n");
}

static const char *graph_kinds[] = GRAPH_KIND_NAMES;
static const char *graph_relations[] = GRAPH_REL_NAMES;

static void graph_request_init(graph_request_t *request, unsigned int op, unsigned long index)
{
    memset(request, 0, sizeof(*request));
    request->op = op;
    request->index = (unsigned int)index;
}

static void copy_name(char *to, const char *from)
{
    unsigned long i = 0;

    while (from[i] && i < GRAPH_NAME_MAX - 1)
    {
        to[i] = from[i];
        i++;
    }

    to[i] = 0;
}

static void print_edge(const graph_edge_info_t *edge)
{
    print("  boot ");
    print_uint(edge->boot);
    print(" +");
    print_uint(edge->uptime / 100);
    print("s  ");
    print(edge->actor);
    print(": ");
    print(graph_kinds[edge->from_kind < GRAPH_KIND_COUNT ? edge->from_kind : 0]);
    print(" ");
    print(edge->from);
    print(" --");
    print(graph_relations[edge->relation < GRAPH_REL_COUNT ? edge->relation : 0]);
    print("--> ");
    print(graph_kinds[edge->to_kind < GRAPH_KIND_COUNT ? edge->to_kind : 0]);
    print(" ");
    print(edge->to);

    if (edge->confidence < 100)
    {
        print(" (");
        print_uint(edge->confidence);
        print("%)");
    }

    print("\n");
}

static int print_node_edges(unsigned int kind, const char *name, unsigned long limit)
{
    graph_request_t request;
    graph_edge_info_t edge;
    unsigned long shown = 0;

    for (unsigned long i = 0; i < limit; i++)
    {
        graph_request_init(&request, GRAPH_OP_EDGES, i);
        request.kind_a = kind;
        copy_name(request.a, name);

        if (graph(&request, &edge) != 0)
        {
            break;
        }

        print_edge(&edge);
        shown++;
    }

    return (int)shown;
}

static void memory_stats(void)
{
    graph_request_t request;
    graph_stats_t stats;

    graph_request_init(&request, GRAPH_OP_STATS, 0);

    long result = graph(&request, &stats);

    if (result != 0)
    {
        fail("memory", result);
        return;
    }

    print("Memory graph: ");
    print_uint(stats.nodes);
    print(" nodes, ");
    print_uint(stats.edges);
    print(" links (room for ");
    print_uint(stats.node_capacity);
    print(" / ");
    print_uint(stats.edge_capacity);
    print("), boot ");
    print_uint(stats.boot);
    print("\n ");

    for (unsigned int kind = 1; kind < GRAPH_KIND_COUNT; kind++)
    {
        if (stats.by_kind[kind])
        {
            print(" ");
            print(graph_kinds[kind]);
            print(" ");
            print_uint(stats.by_kind[kind]);
        }
    }

    print("\n");
}

static void memory_recent(unsigned long count)
{
    graph_request_t request;
    graph_edge_info_t edge;

    for (unsigned long i = 0; i < count; i++)
    {
        graph_request_init(&request, GRAPH_OP_RECENT, i);

        if (graph(&request, &edge) != 0)
        {
            break;
        }

        print_edge(&edge);
    }
}

static void memory_find(const char *text)
{
    graph_request_t request;
    graph_node_info_t node;
    unsigned long found = 0;

    for (unsigned long i = 0; i < 40; i++)
    {
        graph_request_init(&request, GRAPH_OP_FIND, i);
        copy_name(request.a, text);

        if (graph(&request, &node) != 0)
        {
            break;
        }

        print("  ");
        print_padded(graph_kinds[node.kind < GRAPH_KIND_COUNT ? node.kind : 0], 11);
        print(node.name);
        print("  (");
        print_uint(node.edges);
        print(" links, seen ");
        print_uint(node.hits);
        print(" times)\n");
        found++;
    }

    if (!found)
    {
        print("Nothing in memory matches that\n");
    }
}

static void memory_show(const char *name, int forget)
{
    unsigned long found = 0;

    for (unsigned int kind = 1; kind < GRAPH_KIND_COUNT; kind++)
    {
        graph_request_t request;

        if (forget)
        {
            graph_request_init(&request, GRAPH_OP_FORGET, 0);
            request.kind_a = kind;
            copy_name(request.a, name);

            if (graph(&request, 0) == 0)
            {
                print("Forgot ");
                print(graph_kinds[kind]);
                print(" ");
                print(name);
                print("\n");
                found++;
            }

            continue;
        }

        graph_edge_info_t edge;

        graph_request_init(&request, GRAPH_OP_EDGES, 0);
        request.kind_a = kind;
        copy_name(request.a, name);

        if (graph(&request, &edge) != 0)
        {
            continue;
        }

        print(graph_kinds[kind]);
        print(" ");
        print(name);
        print(":\n");
        print_node_edges(kind, name, 30);
        found++;
    }

    if (!found)
    {
        print("Nothing in memory is called ");
        print(name);
        print(" (try: memory find TEXT)\n");
    }
}

static void memory_why(const char *path)
{
    graph_request_t request;
    graph_edge_info_t edge;
    int explained = 0;

    for (unsigned long i = 0; i < 30; i++)
    {
        graph_request_init(&request, GRAPH_OP_EDGES, i);
        request.kind_a = GRAPH_KIND_FILE;
        copy_name(request.a, path);

        if (graph(&request, &edge) != 0)
        {
            break;
        }

        if (edge.relation == GRAPH_REL_MOVED_TO && strcmp(edge.to, path) == 0)
        {
            print(path);
            print(" was moved here by ");
            print(edge.actor);
            print(" from ");
            print(edge.from);
            print(", because:\n");
            print_node_edges(GRAPH_KIND_FILE, edge.from, 10);
            explained = 1;
            break;
        }
    }

    if (!explained)
    {
        print("What KnocOS knows about ");
        print(path);
        print(":\n");

        if (print_node_edges(GRAPH_KIND_FILE, path, 20) == 0)
        {
            print("  nothing yet\n");
        }
    }
}

static void cmd_memory(int argc, char **args)
{
    char path[PATH_MAX];

    if (argc < 2)
    {
        memory_stats();
    }
    else if (strcmp(args[1], "recent") == 0)
    {
        long count = argc > 2 ? parse_number(args[2]) : 10;

        memory_recent(count > 0 ? (unsigned long)count : 10);
    }
    else if (strcmp(args[1], "find") == 0 && argc > 2)
    {
        memory_find(args[2]);
    }
    else if ((strcmp(args[1], "show") == 0 || strcmp(args[1], "forget") == 0) && argc > 2)
    {
        const char *name = args[2];

        if (name[0] == '/' || name[0] == '.')
        {
            resolve(name, path);
            name = path;
        }

        memory_show(name, strcmp(args[1], "forget") == 0);
    }
    else if (strcmp(args[1], "why") == 0 && argc > 2)
    {
        resolve(args[2], path);
        memory_why(path);
    }
    else
    {
        print("usage: memory [recent [N] | find TEXT | show NAME | why FILE | forget NAME]\n");
    }
}

static void cmd_health(void)
{
    telemetry_sample_t s;

    if (telemetry(0, &s) != 0)
    {
        print("health: no telemetry yet\n");
        return;
    }

    print("CPU ");
    print_uint(s.cpu_busy);
    print("%, ");
    print_uint(s.processes);
    print(" processes, RAM ");
    print_uint((s.ram_total_kib - s.ram_free_kib) / 1024);
    print(" / ");
    print_uint(s.ram_total_kib / 1024);
    print(" MiB used, disk ");
    print_uint(s.disk_free_kib / 1024);
    print(" / ");
    print_uint(s.disk_total_kib / 1024);
    print(" MiB free\n");
    print("Per second: ");
    print_uint(s.syscalls);
    print(" system calls, ");
    print_uint(s.disk_reads + s.disk_writes);
    print(" disk requests, ");
    print_uint(s.spawns);
    print(" programs started, ");
    print_uint(s.crashes);
    print(" crashes\n");
    print("Top CPU: ");
    print(s.top_cpu_name[0] ? s.top_cpu_name : "-");
    print(" ");
    print_uint(s.top_cpu);
    print("%   top memory: ");
    print(s.top_mem_name[0] ? s.top_mem_name : "-");
    print(" ");
    print_uint(s.top_mem_kib / 1024);
    print(" MiB   most system calls: ");
    print(s.top_sys_name[0] ? s.top_sys_name : "-");
    print("\n");

    process_info_t info;
    int running = 0;

    for (unsigned long i = 0; ps(i, &info) == 0; i++)
    {
        running |= strcmp(info.name, "healthd") == 0;
    }

    print(running ? "Anomaly detector: running\n" : "Anomaly detector: not running\n");

    graph_request_t request;
    graph_edge_info_t edge;
    int shown = 0;

    for (unsigned long i = 0; i < 400 && shown < 5; i++)
    {
        graph_request_init(&request, GRAPH_OP_RECENT, i);

        if (graph(&request, &edge) != 0)
        {
            break;
        }

        if (edge.relation == GRAPH_REL_ANOMALY)
        {
            if (!shown)
            {
                print("Recent problems:\n");
            }

            print_edge(&edge);
            shown++;
        }
    }

    if (!shown)
    {
        print("No problems recorded\n");
    }
}

static void cmd_uptime(void)
{
    unsigned long ticks = uptime();

    print("up ");
    print_uint(ticks / 100);
    print(" s (");
    print_uint(ticks);
    print(" ticks)\n");
}

static void cmd_help(void)
{
    print("Files:     ls [DIR]  cd DIR  pwd  cat FILE  echo TEXT [> FILE]  mkdir DIR  rm PATH\n");
    print("Programs:  run NAME [&]  or just NAME (programs are in /bin)  ps  kill PID\n");
    print("System:    mem  devices  crashes  ai  health  uptime  sleep N  clear  exit\n");
    print("Memory:    memory  memory recent [N]  memory find TEXT  memory show NAME  memory why FILE  memory forget NAME\n");
    print("AI:        ask QUESTION (the Qwen LLM)  organize DIR\n");
    print("Keys:      Ctrl-C stops the running program, Ctrl-D powers off\n");
}

static int execute(int argc, char **args)
{
    const char *command = args[0];

    if (strcmp(command, "help") == 0)
    {
        cmd_help();
    }
    else if (strcmp(command, "ls") == 0)
    {
        cmd_ls(argc, args);
    }
    else if (strcmp(command, "cd") == 0)
    {
        cmd_cd(argc, args);
    }
    else if (strcmp(command, "pwd") == 0)
    {
        print(cwd);
        print("\n");
    }
    else if (strcmp(command, "cat") == 0)
    {
        cmd_cat(argc, args);
    }
    else if (strcmp(command, "echo") == 0)
    {
        cmd_echo(argc, args);
    }
    else if (strcmp(command, "mkdir") == 0)
    {
        cmd_path_change(argc, args, 1);
    }
    else if (strcmp(command, "rm") == 0)
    {
        cmd_path_change(argc, args, 0);
    }
    else if (strcmp(command, "ps") == 0)
    {
        cmd_ps();
    }
    else if (strcmp(command, "kill") == 0)
    {
        cmd_kill(argc, args);
    }
    else if (strcmp(command, "run") == 0)
    {
        if (argc < 2)
        {
            print("usage: run NAME [&]\n");
        }
        else
        {
            run_program(args[1], strcmp(args[argc - 1], "&") == 0, argc, args, 2);
        }
    }
    else if (strcmp(command, "mem") == 0)
    {
        cmd_mem();
    }
    else if (strcmp(command, "devices") == 0)
    {
        cmd_devices();
    }
    else if (strcmp(command, "crashes") == 0)
    {
        cmd_crashes();
    }
    else if (strcmp(command, "ai") == 0)
    {
        cmd_ai();
    }
    else if (strcmp(command, "memory") == 0)
    {
        cmd_memory(argc, args);
    }
    else if (strcmp(command, "sleep") == 0)
    {
        long seconds = argc > 1 ? parse_number(args[1]) : -1;

        if (seconds < 0)
        {
            print("usage: sleep SECONDS\n");
        }
        else
        {
            sleep((unsigned long)seconds * 100);
        }
    }
    else if (strcmp(command, "health") == 0)
    {
        cmd_health();
    }
    else if (strcmp(command, "uptime") == 0)
    {
        cmd_uptime();
    }
    else if (strcmp(command, "clear") == 0)
    {
        print("\033[2J\033[H");
    }
    else if (strcmp(command, "exit") == 0)
    {
        return 1;
    }
    else if (program_exists(command))
    {
        run_program(command, argc > 1 && strcmp(args[argc - 1], "&") == 0, argc, args, 1);
    }
    else
    {
        print("knocsh: unknown command: ");
        print(command);
        print(" (type help)\n");
    }

    return 0;
}

int main(void)
{
    char *args[WORDS_MAX];

    print("\nKnocOS shell (knocsh). Type help for commands.\n");

    while (1)
    {
        print("knoc:");
        print(cwd);
        print("$ ");

        read_line();

        int argc = split(line, args);

        if (argc > 0 && execute(argc, args))
        {
            print("knocsh: bye (the console goes back to plain echo)\n");
            return 0;
        }
    }
}
