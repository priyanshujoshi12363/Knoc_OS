#include "ulib.h"

/* knocsh: the KnocOS shell. Reads a line, runs a built-in command or a
   program from /bin, and waits for it. Everything goes through system
   calls: the shell is an ordinary user program. */

#define LINE_MAX 256
#define WORDS_MAX 32
#define PARTS_MAX 32
#define CAT_CHUNK 256
#define MIB (1024UL * 1024)
#define VARS_MAX 32
#define VAR_NAME 16
#define VAR_VALUE 128
#define SCRIPT_ARGS 10
#define SCRIPT_MAX 8192
#define SCRIPT_LINES 256
#define SCRIPT_DEPTH 4
#define LOOP_MAX 10000
#define STARTUP_SCRIPT "/etc/startup.ksh"
#define COPY_CHUNK 4096

static char cwd[PATH_MAX] = "/";
static char line[LINE_MAX];

typedef struct variable
{
    char name[VAR_NAME];
    char value[VAR_VALUE];
} variable_t;

typedef struct frame
{
    int count;
    char args[SCRIPT_ARGS][VAR_VALUE];
} frame_t;

typedef struct script
{
    char *lines[SCRIPT_LINES];
    int count;
} script_t;

static variable_t variables[VARS_MAX];
static frame_t frames[SCRIPT_DEPTH];
static script_t scripts[SCRIPT_DEPTH];
static char script_text[SCRIPT_DEPTH][SCRIPT_MAX];
static frame_t *frame;
static int depth;
static int status;
static int stop_script;
static int redirect_fd = -1;

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
        write(output(), &digits[--count], 1);
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

static void fail(const char *what, long code)
{
    status = 1;
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

static void put_two(char *out, unsigned long value)
{
    out[0] = (char)('0' + value / 10 % 10);
    out[1] = (char)('0' + value % 10);
}

static void print_date(unsigned long seconds)
{
    char text[17] = "----------------";

    if (seconds > 0)
    {
        long days = (long)(seconds / 86400) + 719468;
        long era = days / 146097;
        unsigned long doe = (unsigned long)(days - era * 146097);
        unsigned long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        unsigned long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        unsigned long mp = (5 * doy + 2) / 153;
        unsigned long day = doy - (153 * mp + 2) / 5 + 1;
        unsigned long month = mp < 10 ? mp + 3 : mp - 9;
        unsigned long year = yoe + (unsigned long)era * 400 + (month <= 2);
        unsigned long clock = seconds % 86400;

        put_two(text, year / 100);
        put_two(text + 2, year);
        text[4] = '-';
        put_two(text + 5, month);
        text[7] = '-';
        put_two(text + 8, day);
        text[10] = ' ';
        put_two(text + 11, clock / 3600);
        text[13] = ':';
        put_two(text + 14, clock / 60 % 60);
    }

    text[16] = 0;
    print(text);
    print("  ");
}

static void cmd_ls(int argc, char **args)
{
    char path[PATH_MAX];
    file_stat_t info;
    dir_entry_t entry;
    int long_form = argc > 1 && strcmp(args[1], "-l") == 0;

    if (long_form)
    {
        args++;
        argc--;
    }

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

        if (long_form)
        {
            print_date(info.modified);
        }

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

            if (long_form)
            {
                print_date(entry.modified);
            }

            print(entry.name);
            print("/\n");
        }
        else
        {
            print_size(entry.size);
            print("  ");

            if (long_form)
            {
                print_date(entry.modified);
            }

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
        chdir(path);
        graph_record(GRAPH_KIND_ACTOR, "user", GRAPH_REL_WORKED_IN, GRAPH_KIND_FOLDER, path, 100);
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
        write(output(), buffer, (unsigned long)count);
        newline = buffer[count - 1] == '\n';
    }

    close(fd);

    if (!newline)
    {
        print("\n");
    }
}

static void cmd_echo(int argc, char **args)
{
    char text[LINE_MAX];
    unsigned long length = 0;

    for (int i = 1; i < argc; i++)
    {
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
    write(output(), text, length);
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

static void destination(const char *from, const char *to, char *out)
{
    file_stat_t info;

    resolve(to, out);

    if (stat(out, &info) == 0 && info.type == FILE_TYPE_DIR && strlen(out) + strlen(base_name(from)) + 2 < PATH_MAX)
    {
        if (strcmp(out, "/") != 0)
        {
            strcpy(out + strlen(out), "/");
        }

        strcpy(out + strlen(out), base_name(from));
    }
}

static void cmd_copy(int argc, char **args, int move)
{
    char from[PATH_MAX];
    char to[PATH_MAX];
    static char chunk[COPY_CHUNK];

    if (argc < 3)
    {
        print(move ? "usage: move FROM TO\n" : "usage: copy FROM TO\n");
        status = 1;
        return;
    }

    resolve(args[1], from);
    destination(from, args[2], to);

    if (move)
    {
        long result = rename(from, to);

        if (result != 0)
        {
            fail(result == E_EXISTS ? to : from, result);
        }
        else
        {
            graph_record(GRAPH_KIND_FILE, from, GRAPH_REL_MOVED_TO, GRAPH_KIND_FILE, to, 100);
        }

        return;
    }

    int in = open(from, O_READ);

    if (in < 0)
    {
        fail(from, in);
        return;
    }

    int out = open(to, O_WRITE | O_CREATE | O_TRUNC);

    if (out < 0)
    {
        close(in);
        fail(to, out);
        return;
    }

    long got;

    while ((got = read(in, chunk, sizeof(chunk))) > 0)
    {
        if (write(out, chunk, (unsigned long)got) != got)
        {
            fail(to, E_NOSPACE);
            break;
        }
    }

    close(in);
    close(out);
}

static variable_t *find_variable(const char *name, int create)
{
    for (int i = 0; i < VARS_MAX; i++)
    {
        if (variables[i].name[0] && strcmp(variables[i].name, name) == 0)
        {
            return &variables[i];
        }
    }

    for (int i = 0; create && i < VARS_MAX; i++)
    {
        if (variables[i].name[0] == 0)
        {
            copy_text(variables[i].name, name, VAR_NAME);
            return &variables[i];
        }
    }

    return 0;
}

static void set_variable(const char *name, const char *value)
{
    variable_t *v = find_variable(name, 1);

    if (v)
    {
        copy_text(v->value, value, VAR_VALUE);
    }
    else
    {
        print("knocsh: too many variables\n");
        status = 1;
    }
}

static void cmd_set(int argc, char **args)
{
    char value[VAR_VALUE];
    unsigned long length = 0;

    if (argc < 2)
    {
        for (int i = 0; i < VARS_MAX; i++)
        {
            if (variables[i].name[0])
            {
                print(variables[i].name);
                print("=");
                print(variables[i].value);
                print("\n");
            }
        }

        return;
    }

    for (int i = 2; i < argc; i++)
    {
        if (i > 2 && length < sizeof(value) - 1)
        {
            value[length++] = ' ';
        }

        for (unsigned long j = 0; args[i][j] && length < sizeof(value) - 1; j++)
        {
            value[length++] = args[i][j];
        }
    }

    value[length] = 0;
    set_variable(args[1], value);
}

static void cmd_inc(int argc, char **args)
{
    char text[24];
    variable_t *v = argc > 1 ? find_variable(args[1], 1) : 0;

    if (!v)
    {
        print("usage: inc NAME\n");
        status = 1;
        return;
    }

    long value = parse_number(v->value);
    long step = argc > 2 ? parse_number(args[2]) : 1;
    unsigned long n = (unsigned long)((value < 0 ? 0 : value) + (step < 0 ? 0 : step));
    int length = 0;
    char digits[24];

    do
    {
        digits[length++] = (char)('0' + n % 10);
        n /= 10;
    } while (n);

    for (int i = 0; i < length; i++)
    {
        text[i] = digits[length - 1 - i];
    }

    text[length] = 0;
    copy_text(v->value, text, VAR_VALUE);
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

    print("  PID  NAME          CLASS        STATE     CPU  MEMORY    MODE    CORE\n");

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
        print_padded(info.user ? ((info.flags & PROCESS_FLAG_LINUX) ? "linux" : "user") : "kernel", 8);

        if (info.reserved > 0 && info.pid != 0)
        {
            print_uint(info.reserved - 1);
        }

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

static int usr_bin_path(const char *name, char *path)
{
    file_stat_t info;

    for (unsigned long i = 0; name[i]; i++)
    {
        if (name[i] == '/')
        {
            return 0;
        }
    }

    if (strlen(name) >= PATH_MAX - 10)
    {
        return 0;
    }

    strcpy(path, "/usr/bin/");
    strcpy(path + 9, name);
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

static char applets[4096];
static int applets_loaded;

static int busybox_applet(const char *command)
{
    if (!applets_loaded)
    {
        applets_loaded = 1;

        if (!program_exists("busybox"))
        {
            return 0;
        }

        int pid = spawn_capture("busybox", "--list", 1);

        if (pid >= 0)
        {
            wait(pid);

            long length = captured(applets, sizeof(applets) - 1);

            applets[length > 0 ? length : 0] = 0;
        }
    }

    unsigned long n = strlen(command);

    for (char *p = applets; *p;)
    {
        char *end = p;

        while (*end && *end != '\n')
        {
            end++;
        }

        unsigned long i = 0;

        while (i < n && p + i < end && p[i] == command[i])
        {
            i++;
        }

        if (i == n && p + n == end)
        {
            return 1;
        }

        p = *end ? end + 1 : end;
    }

    return 0;
}

static void run_program(const char *name, int background, int argc, char **args, int first)
{
    char program_args[ARGS_MAX];

    join_args(argc, args, first, program_args);

    int capture = redirect_fd >= 0 && !background;
    int pid = capture ? spawn_capture(name, program_args, 1) : spawn_args(name, program_args);

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

    if (capture)
    {
        static char captured_output[4096];
        long length = captured(captured_output, sizeof(captured_output));

        if (length > 0)
        {
            write(redirect_fd, captured_output, (unsigned long)length);
        }
    }

    status = code == 0 ? 0 : code > 0 ? (int)code : 1;

    if (code == E_KILLED && depth > 0)
    {
        stop_script = 1;
    }

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

    print("AI space: online on core 4, running for ");
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

static int health_recovers(void)
{
    char text[16];
    int fd = open("/etc/health.mode", O_READ);

    if (fd < 0)
    {
        return 1;
    }

    long got = read(fd, text, sizeof(text) - 1);

    close(fd);
    return !(got >= 5 && memcmp_bytes(text, "watch", 5) == 0);
}

static void cmd_health_mode(const char *mode)
{
    if (strcmp(mode, "watch") != 0 && strcmp(mode, "recover") != 0)
    {
        print("usage: health [watch | recover]\n");
        return;
    }

    mkdir("/etc");

    int fd = open("/etc/health.mode", O_WRITE | O_CREATE | O_TRUNC);

    if (fd < 0)
    {
        print("health: cannot write /etc/health.mode\n");
        return;
    }

    write(fd, mode, strlen(mode));
    close(fd);
    print(strcmp(mode, "watch") == 0 ? "healthd will only report problems\n"
                                     : "healthd will report problems and fix them\n");
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
    print("   most disk: ");
    print(s.top_disk_name[0] ? s.top_disk_name : "-");
    print("\n");

    process_info_t info;
    int running = 0;

    for (unsigned long i = 0; ps(i, &info) == 0; i++)
    {
        running |= strcmp(info.name, "healthd") == 0;
    }

    print(running ? "Anomaly detector: running, " : "Anomaly detector: not running, ");
    print(health_recovers() ? "recover mode (fixes problems itself)\n" : "watch mode (only reports)\n");

    graph_request_t request;
    graph_edge_info_t edge;
    int shown = 0;

    for (unsigned long i = 0; i < 400 && shown < 8; i++)
    {
        graph_request_init(&request, GRAPH_OP_RECENT, i);

        if (graph(&request, &edge) != 0)
        {
            break;
        }

        if (edge.relation == GRAPH_REL_ANOMALY ||
            (strcmp(edge.actor, "healthd") == 0 &&
             (edge.relation == GRAPH_REL_STOPPED || edge.relation == GRAPH_REL_LOWERED)))
        {
            if (!shown)
            {
                print("Recent problems and fixes:\n");
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

#define CONTEXT_SCAN 600
#define CONTEXT_ITEMS 24
#define CONTEXT_SHOWN 5

typedef struct tally
{
    char name[GRAPH_NAME_MAX];
    unsigned int count;
} tally_t;

static tally_t context_folders[CONTEXT_ITEMS];
static tally_t context_programs[CONTEXT_ITEMS];

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

static int system_program(const char *name)
{
    return strcmp(name, "knocsh") == 0 || strcmp(name, "healthd") == 0 || strcmp(name, "organized") == 0 ||
           strcmp(name, "recorder") == 0 || strcmp(name, "knocnetd") == 0;
}

static void tally_print(const char *title, tally_t *items, const char *unit)
{
    int shown = 0;

    print(title);

    for (int n = 0; n < CONTEXT_SHOWN; n++)
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

        print(shown ? ", " : " ");
        print(items[best].name);
        print(" (");
        print_uint(items[best].count);
        print(unit);
        print(")");
        items[best].count = 0;
        shown++;
    }

    print(shown ? "\n" : " nothing yet\n");
}

static void cmd_context(void)
{
    graph_request_t request;
    graph_edge_info_t edge;
    char folder[PATH_MAX];
    unsigned int problems = 0;
    unsigned int actions = 0;
    unsigned int boot = 0;

    memset(context_folders, 0, sizeof(context_folders));
    memset(context_programs, 0, sizeof(context_programs));

    for (unsigned long i = 0; i < CONTEXT_SCAN; i++)
    {
        graph_request_init(&request, GRAPH_OP_RECENT, i);

        if (graph(&request, &edge) != 0)
        {
            break;
        }

        if (i == 0)
        {
            boot = edge.boot;
        }

        if (edge.relation == GRAPH_REL_WORKED_IN)
        {
            if (edge.to_kind == GRAPH_KIND_FOLDER)
            {
                tally_add(context_folders, edge.to, 3);
            }
            else
            {
                folder_part(edge.to, folder);
                tally_add(context_folders, folder, 1);
            }

            actions++;
        }
        else if (edge.relation == GRAPH_REL_MOVED_TO && strcmp(edge.actor, "organize") != 0)
        {
            folder_part(edge.to, folder);
            tally_add(context_folders, folder, 1);
            actions++;
        }
        else if (edge.relation == GRAPH_REL_STARTED && strcmp(edge.from, "knocsh") == 0 && !system_program(edge.to))
        {
            tally_add(context_programs, edge.to, 1);
            actions++;
        }
        else if (edge.relation == GRAPH_REL_ANOMALY)
        {
            problems++;
        }
    }

    print("Your recent work (from the memory graph, boot ");
    print_uint(boot);
    print("):\n");
    tally_print("  Folders you work in:", context_folders, "");
    tally_print("  Programs you use:   ", context_programs, "x");
    print("  Actions counted: ");
    print_uint(actions);
    print(", problems healthd found: ");
    print_uint(problems);
    print("\n");
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

static void cmd_cpus(void)
{
    static const char *roles[] = {"general", "AI", "AI space"};
    cpu_info_t before[8];
    cpu_info_t after;

    for (unsigned long i = 0; i < 8; i++)
    {
        if (cpuinfo(i, &before[i]) != 0)
        {
            before[i].online = 0;
        }
    }

    sleep(50);
    print("  CORE  ROLE      LOAD  RUNNING\n");

    for (unsigned long i = 0; i < 8 && cpuinfo(i, &after) == 0; i++)
    {
        print("  ");
        print_padded_uint(i, 6);
        print_padded(after.role < 3 ? roles[after.role] : "?", 10);

        if (!after.online)
        {
            print("off\n");
            continue;
        }

        if (after.role == CPU_ROLE_AI_SPACE)
        {
            print("-     guardian: crash diagnosis, black box, warm restart\n");
            continue;
        }

        unsigned long busy = after.busy_ticks - before[i].busy_ticks;
        unsigned long idle = after.idle_ticks - before[i].idle_ticks;
        unsigned long load = busy + idle > 0 ? busy * 100 / (busy + idle) : 0;
        char percent[8];
        unsigned long n = 0;

        if (load >= 100)
        {
            percent[n++] = '1';
            percent[n++] = '0';
            percent[n++] = '0';
        }
        else
        {
            if (load >= 10)
            {
                percent[n++] = (char)('0' + load / 10);
            }

            percent[n++] = (char)('0' + load % 10);
        }

        percent[n++] = '%';
        percent[n] = 0;
        print_padded(percent, 6);
        print(after.running_pid >= 0 ? after.running : "-");
        print("\n");
    }
}

static void cmd_help(void)
{
    print("Files:     ls [-l] [DIR]  cd DIR  pwd  cat FILE  echo TEXT  mkdir DIR  rm PATH  copy FROM TO  move FROM TO\n");
    print("Output:    COMMAND > FILE  COMMAND >> FILE (works for every command and program)\n");
    print("Scripts:   run FILE.ksh [ARGS] or FILE.ksh  set NAME VALUE  inc NAME  $NAME $1 $# $?\n");
    print("           if COND / else / end  for X in A B C / end  while COND / end  exit N\n");
    print("           COND: exists PATH, A == B, A != B, not COND, or any command (true when it exits with 0)\n");
    print("Programs:  run NAME [&]  or just NAME (programs are in /bin)  ps  kill PID\n");
    print("System:    mem  cpus  devices  crashes  ai  health [watch|recover]  context  uptime  sleep N  clear  exit\n");
    print("Memory:    memory  memory recent [N]  memory find TEXT  memory show NAME  memory why FILE  memory forget NAME\n");
    print("AI:        chat  ask QUESTION  agent TASK  agent --tools  organize DIR\n");
    print("Organize:  organize auto on|off  organize learn  organize personal  organize forget\n");
    print("Code:      tcc FILE.c -o NAME  then ./NAME   (C compiler with the standard C library)\n");
    print("Network:   net  ping HOST [COUNT]  fetch URL [FILE]  web URL  web -s WORDS  date\n");
    print("           (http:// and https://; KnocOS is 10.0.2.15, your PC is 10.0.2.2 = host)\n");
    print("Linux:     Linux programs run too (static RISC-V): ./program, or put them in /bin\n");
    print("           with BusyBox in /bin, its tools work directly: grep, sed, awk, tar, vi, top, df...\n");
    print("KnocNet:   knocnet id | pair wait | pair ADDRESS CODE | peers | ping | status | send | get | ask\n");
    print("Keys:      Ctrl-C stops the running program, Ctrl-D powers off\n");
}

static int is_name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static void put_number(char *out, unsigned long *length, unsigned long room, long value)
{
    char digits[24];
    int n = 0;
    unsigned long v = (unsigned long)(value < 0 ? -value : value);

    if (value < 0 && *length < room - 1)
    {
        out[(*length)++] = '-';
    }

    do
    {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);

    while (n > 0 && *length < room - 1)
    {
        out[(*length)++] = digits[--n];
    }
}

static void expand(const char *in, char *out, unsigned long room)
{
    unsigned long length = 0;
    int single = 0;

    for (unsigned long i = 0; in[i] && length < room - 1;)
    {
        if (in[i] == '\'')
        {
            single = !single;
            out[length++] = in[i++];
            continue;
        }

        if (single)
        {
            out[length++] = in[i++];
            continue;
        }

        if (in[i] == '\\' && in[i + 1] == '$')
        {
            out[length++] = '$';
            i += 2;
            continue;
        }

        if (in[i] != '$')
        {
            out[length++] = in[i++];
            continue;
        }

        char next = in[i + 1];
        const char *value = 0;

        if (next >= '0' && next <= '9')
        {
            int index = next - '0';

            value = frame && index < frame->count ? frame->args[index] : "";
            i += 2;
        }
        else if (next == '#')
        {
            put_number(out, &length, room, frame ? frame->count - 1 : 0);
            i += 2;
            continue;
        }
        else if (next == '?')
        {
            put_number(out, &length, room, status);
            i += 2;
            continue;
        }
        else if (is_name_char(next))
        {
            char name[VAR_NAME];
            int n = 0;

            i++;

            while (is_name_char(in[i]))
            {
                if (n < VAR_NAME - 1)
                {
                    name[n++] = in[i];
                }

                i++;
            }

            name[n] = 0;

            variable_t *v = find_variable(name, 0);

            value = v ? v->value : "";
        }
        else
        {
            out[length++] = in[i++];
            continue;
        }

        while (*value && length < room - 1)
        {
            out[length++] = *value++;
        }
    }

    out[length] = 0;
}

static int execute(int argc, char **args);

static int open_redirect(int argc, char **args, int *fd)
{
    if (argc < 3 || (strcmp(args[argc - 2], ">") != 0 && strcmp(args[argc - 2], ">>") != 0))
    {
        return argc;
    }

    char path[PATH_MAX];
    int append = strcmp(args[argc - 2], ">>") == 0;

    resolve(args[argc - 1], path);
    *fd = open(path, O_WRITE | O_CREATE | (append ? 0 : O_TRUNC));

    if (*fd < 0)
    {
        fail(path, *fd);
        return -1;
    }

    graph_record(GRAPH_KIND_ACTOR, "user", GRAPH_REL_WORKED_IN, GRAPH_KIND_FILE, path, 100);

    if (append)
    {
        file_stat_t info;

        if (stat(path, &info) == 0)
        {
            seek(*fd, info.size);
        }
    }

    return argc - 2;
}

static int run_words(int argc, char **args)
{
    int fd = -1;

    argc = open_redirect(argc, args, &fd);

    if (argc <= 0)
    {
        return 0;
    }

    if (fd >= 0)
    {
        set_output(fd);
        redirect_fd = fd;
    }

    int result = execute(argc, args);

    if (fd >= 0)
    {
        set_output(FD_STDOUT);
        redirect_fd = -1;
        close(fd);
    }

    return result;
}

static int run_line(const char *text)
{
    char expanded[LINE_MAX];
    char *args[WORDS_MAX];

    expand(text, expanded, sizeof(expanded));

    int argc = split(expanded, args);

    return argc > 0 ? run_words(argc, args) : 0;
}

static const char *skip_spaces(const char *text)
{
    while (*text == ' ' || *text == '\t')
    {
        text++;
    }

    return text;
}

static int first_word_is(const char *text, const char *word)
{
    unsigned long n = strlen(word);

    text = skip_spaces(text);
    return memcmp_bytes(text, word, n) == 0 && (text[n] == 0 || text[n] == ' ' || text[n] == '\t');
}

static int block_start(const char *text)
{
    return first_word_is(text, "if") || first_word_is(text, "for") || first_word_is(text, "while");
}

static int find_end(script_t *s, int start, int *else_at)
{
    int nest = 0;

    *else_at = -1;

    for (int i = start + 1; i < s->count; i++)
    {
        if (block_start(s->lines[i]))
        {
            nest++;
        }
        else if (first_word_is(s->lines[i], "end"))
        {
            if (nest == 0)
            {
                return i;
            }

            nest--;
        }
        else if (first_word_is(s->lines[i], "else") && nest == 0)
        {
            *else_at = i;
        }
    }

    return -1;
}

static int condition(const char *text)
{
    char expanded[LINE_MAX];
    char *args[WORDS_MAX];

    expand(text, expanded, sizeof(expanded));

    int argc = split(expanded, args);
    int negate = argc > 0 && strcmp(args[0], "not") == 0;
    char **words = args + negate;
    int count = argc - negate;
    int result;

    if (count == 2 && strcmp(words[0], "exists") == 0)
    {
        char path[PATH_MAX];
        file_stat_t info;

        resolve(words[1], path);
        result = stat(path, &info) == 0;
    }
    else if (count == 3 && (strcmp(words[1], "==") == 0 || strcmp(words[1], "!=") == 0))
    {
        result = (strcmp(words[0], words[2]) == 0) == (strcmp(words[1], "==") == 0);
    }
    else if (count == 2 && (strcmp(words[0], "==") == 0 || strcmp(words[0], "!=") == 0))
    {
        result = (words[1][0] == 0) == (strcmp(words[0], "==") == 0);
    }
    else if (count > 0)
    {
        run_words(count, words);
        result = status == 0;
    }
    else
    {
        result = 0;
    }

    return negate ? !result : result;
}

static void script_error(int line_number, const char *what)
{
    status = 1;
    print("knocsh: script line ");
    print_uint((unsigned long)line_number + 1);
    print(": ");
    print(what);
    print("\n");
}

static int run_range(script_t *s, int from, int to)
{
    for (int i = from; i < to; i++)
    {
        const char *text = skip_spaces(s->lines[i]);

        if (text[0] == 0 || text[0] == '#')
        {
            continue;
        }

        if (block_start(text))
        {
            int else_at;
            int end = find_end(s, i, &else_at);

            if (end < 0 || end > to)
            {
                script_error(i, "missing end");
                return 1;
            }

            if (first_word_is(text, "if"))
            {
                int result = condition(skip_spaces(text + 2));

                if (stop_script)
                {
                    return 1;
                }

                if (result && run_range(s, i + 1, else_at >= 0 ? else_at : end))
                {
                    return 1;
                }

                if (!result && else_at >= 0 && run_range(s, else_at + 1, end))
                {
                    return 1;
                }
            }
            else if (first_word_is(text, "for"))
            {
                char expanded[LINE_MAX];
                char *words[WORDS_MAX];

                expand(skip_spaces(text + 3), expanded, sizeof(expanded));

                int count = split(expanded, words);

                if (count < 2 || strcmp(words[1], "in") != 0)
                {
                    script_error(i, "use: for NAME in WORDS...");
                    return 1;
                }

                for (int w = 2; w < count; w++)
                {
                    set_variable(words[0], words[w]);

                    if (run_range(s, i + 1, end))
                    {
                        return 1;
                    }
                }
            }
            else
            {
                int rounds = 0;

                while (condition(skip_spaces(text + 5)))
                {
                    if (stop_script || run_range(s, i + 1, end))
                    {
                        return 1;
                    }

                    if (++rounds >= LOOP_MAX)
                    {
                        script_error(i, "while loop stopped after 10000 rounds");
                        return 1;
                    }
                }
            }

            i = end;
            continue;
        }

        if (first_word_is(text, "else") || first_word_is(text, "end"))
        {
            script_error(i, "else or end without if, for or while");
            return 1;
        }

        if (first_word_is(text, "exit"))
        {
            char expanded[LINE_MAX];

            expand(skip_spaces(text + 4), expanded, sizeof(expanded));
            status = expanded[0] ? (int)parse_number(expanded) : status;
            return 1;
        }

        run_line(text);

        if (stop_script)
        {
            return 1;
        }
    }

    return 0;
}

static void run_script(const char *path, int argc, char **args, int first)
{
    if (depth >= SCRIPT_DEPTH)
    {
        print("knocsh: scripts nested too deep\n");
        status = 1;
        return;
    }

    int fd = open(path, O_READ);

    if (fd < 0)
    {
        fail(path, fd);
        return;
    }

    char *text = script_text[depth];
    long length = read(fd, text, SCRIPT_MAX - 1);

    close(fd);

    if (length < 0)
    {
        fail(path, length);
        return;
    }

    text[length] = 0;

    script_t *s = &scripts[depth];
    frame_t *f = &frames[depth];

    s->count = 0;

    for (char *p = text; *p && s->count < SCRIPT_LINES;)
    {
        s->lines[s->count++] = p;

        while (*p && *p != '\n')
        {
            p++;
        }

        if (*p)
        {
            *p++ = 0;
        }
    }

    for (int i = 0; i < s->count; i++)
    {
        unsigned long n = strlen(s->lines[i]);

        if (n > 0 && s->lines[i][n - 1] == '\r')
        {
            s->lines[i][n - 1] = 0;
        }
    }

    f->count = 0;
    copy_text(f->args[f->count++], path, VAR_VALUE);

    for (int i = first; i < argc && f->count < SCRIPT_ARGS; i++)
    {
        copy_text(f->args[f->count++], args[i], VAR_VALUE);
    }

    frame_t *saved = frame;

    frame = f;
    depth++;
    status = 0;
    run_range(s, 0, s->count);
    depth--;
    frame = saved;

    if (stop_script)
    {
        print("knocsh: script ");
        print(path);
        print(" stopped\n");

        if (depth == 0)
        {
            stop_script = 0;
        }
    }
}

static int strchr_char(const char *text, char c)
{
    for (; *text; text++)
    {
        if (*text == c)
        {
            return 1;
        }
    }

    return 0;
}

static int executable_path(const char *word, char *path)
{
    unsigned char magic[4];

    resolve(word, path);

    int fd = open(path, O_READ);

    if (fd < 0)
    {
        return 0;
    }

    long got = read(fd, magic, 4);

    close(fd);
    return got == 4 && magic[0] == 0x7F && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F';
}

static int script_path(const char *word, char *path)
{
    unsigned long n = strlen(word);
    int slash = 0;
    file_stat_t info;

    for (unsigned long i = 0; i < n; i++)
    {
        slash |= word[i] == '/';
    }

    if (!slash && !(n > 4 && strcmp(word + n - 4, ".ksh") == 0))
    {
        return 0;
    }

    resolve(word, path);
    return stat(path, &info) == 0 && info.type == FILE_TYPE_FILE;
}

static int execute(int argc, char **args)
{
    const char *command = args[0];
    char path[PATH_MAX];

    status = 0;

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
    else if (strcmp(command, "copy") == 0 || strcmp(command, "cp") == 0)
    {
        cmd_copy(argc, args, 0);
    }
    else if (strcmp(command, "move") == 0 || strcmp(command, "mv") == 0)
    {
        cmd_copy(argc, args, 1);
    }
    else if (strcmp(command, "set") == 0)
    {
        cmd_set(argc, args);
    }
    else if (strcmp(command, "inc") == 0)
    {
        cmd_inc(argc, args);
    }
    else if (block_start(command) || strcmp(command, "else") == 0 || strcmp(command, "end") == 0)
    {
        print("knocsh: ");
        print(command);
        print(" works inside scripts (.ksh files)\n");
        status = 1;
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
            print("usage: run NAME [&] | run SCRIPT.ksh [ARGS]\n");
            status = 1;
        }
        else if (script_path(args[1], path))
        {
            run_script(path, argc, args, 2);
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
        if (argc > 1)
        {
            cmd_health_mode(args[1]);
        }
        else
        {
            cmd_health();
        }
    }
    else if (strcmp(command, "uptime") == 0)
    {
        cmd_uptime();
    }
    else if (strcmp(command, "cpus") == 0)
    {
        cmd_cpus();
    }
    else if (strcmp(command, "context") == 0)
    {
        cmd_context();
    }
    else if (strcmp(command, "clear") == 0)
    {
        print("\033[2J\033[H");
    }
    else if (strcmp(command, "exit") == 0)
    {
        return 1;
    }
    else if (strchr_char(command, '/') && executable_path(command, path))
    {
        run_program(path, argc > 1 && strcmp(args[argc - 1], "&") == 0, argc, args, 1);
    }
    else if (script_path(command, path))
    {
        run_script(path, argc, args, 1);
    }
    else if (program_exists(command))
    {
        run_program(command, argc > 1 && strcmp(args[argc - 1], "&") == 0, argc, args, 1);
    }
    else if (usr_bin_path(command, path))
    {
        run_program(path, argc > 1 && strcmp(args[argc - 1], "&") == 0, argc, args, 1);
    }
    else if (busybox_applet(command))
    {
        run_program("busybox", argc > 1 && strcmp(args[argc - 1], "&") == 0, argc, args, 0);
    }
    else
    {
        print("knocsh: unknown command: ");
        print(command);
        print(" (type help)\n");
        status = 1;
    }

    return 0;
}

int main(void)
{
    char request[ARGS_MAX];
    char *args[WORDS_MAX];
    char path[PATH_MAX];
    file_stat_t info;

    getargs(request, sizeof(request));

    int argc = split(request, args);

    if (argc > 0)
    {
        resolve(args[0], path);
        run_script(path, argc, args, 1);
        return status;
    }

    print("\nKnocOS shell (knocsh). Type help for commands.\n");

    if (stat(STARTUP_SCRIPT, &info) == 0)
    {
        run_script(STARTUP_SCRIPT, 0, args, 0);
    }

    while (1)
    {
        print("knoc:");
        print(cwd);
        print("$ ");

        read_line();

        if (run_line(line))
        {
            print("knocsh: bye (the console goes back to plain echo)\n");
            return 0;
        }
    }
}
