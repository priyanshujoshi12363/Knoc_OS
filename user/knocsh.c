#include "ulib.h"

/* knocsh: the KnocOS shell. Reads a line, runs a built-in command or a
   program from /bin, and waits for it. Everything goes through system
   calls: the shell is an ordinary user program. */

#define LINE_MAX 128
#define ARGS_MAX 8
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

    while (*text && count < ARGS_MAX)
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
    long pid = argc > 1 ? parse_number(args[1]) : -1;

    if (pid < 0)
    {
        print("usage: kill PID\n");
        return;
    }

    long result = kill((int)pid);

    if (result != 0)
    {
        fail(result == E_PERM ? "kill (only user programs can be stopped)" : "kill", result);
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

static void run_program(const char *name, int background)
{
    int pid = spawn(name);

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
    print("System:    mem  devices  crashes  ai  uptime  clear  exit\n");
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
            run_program(args[1], argc > 2 && strcmp(args[2], "&") == 0);
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
        run_program(command, argc > 1 && strcmp(args[argc - 1], "&") == 0);
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
    char *args[ARGS_MAX];

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
