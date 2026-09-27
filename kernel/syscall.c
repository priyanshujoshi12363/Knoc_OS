#include "syscall.h"
#include "syscall_abi.h"
#include "process.h"
#include "program.h"
#include "device.h"
#include "timer.h"
#include "uart.h"
#include "vm.h"
#include "page.h"
#include "string.h"
#include "knocfs.h"
#include "tty.h"
#include "guardian.h"
#include "memgraph.h"
#include "telemetry.h"
#include "net.h"
#include "heap.h"
#include "rtc.h"
#include "virtio_rng.h"
#include "linux.h"
#include "screen.h"
#include "pty.h"
#include "virtio_gpu.h"

#define SYSCALL_WRITE_MAX 4096
#define SYSCALL_CHUNK 64
#define SYSCALL_SLEEP_MAX 6000
#define SYSCALL_FILE_IO_MAX (16UL * 1024 * 1024)
#define FILE_RUN_MAX (1024UL * 1024)

static const char *syscall_names[SYS_COUNT] = {
    [SYS_EXIT] = "exit",
    [SYS_WRITE] = "write",
    [SYS_READ] = "read",
    [SYS_GETPID] = "getpid",
    [SYS_YIELD] = "yield",
    [SYS_SLEEP] = "sleep",
    [SYS_UPTIME] = "uptime",
    [SYS_SPAWN] = "spawn",
    [SYS_MEM_ALLOC] = "mem_alloc",
    [SYS_OPEN] = "open",
    [SYS_CLOSE] = "close",
    [SYS_SEEK] = "seek",
    [SYS_STAT] = "stat",
    [SYS_READDIR] = "readdir",
    [SYS_MKDIR] = "mkdir",
    [SYS_REMOVE] = "remove",
    [SYS_WAIT] = "wait",
    [SYS_PS] = "ps",
    [SYS_KILL] = "kill",
    [SYS_SYSINFO] = "sysinfo",
    [SYS_DEVINFO] = "devinfo",
    [SYS_CRASHINFO] = "crashinfo",
    [SYS_GETARGS] = "getargs",
    [SYS_RENAME] = "rename",
    [SYS_GRAPH] = "graph",
    [SYS_TELEMETRY] = "telemetry",
    [SYS_SETCLASS] = "setclass",
    [SYS_SPAWN_CAPTURE] = "spawn_capture",
    [SYS_CAPTURED] = "captured",
    [SYS_CHDIR] = "chdir",
    [SYS_GETCWD] = "getcwd",
    [SYS_NET_INFO] = "net_info",
    [SYS_NET_RESOLVE] = "net_resolve",
    [SYS_NET_PING] = "net_ping",
    [SYS_TCP_CONNECT] = "tcp_connect",
    [SYS_TCP_SEND] = "tcp_send",
    [SYS_TCP_RECV] = "tcp_recv",
    [SYS_TCP_CLOSE] = "tcp_close",
    [SYS_TIME] = "time",
    [SYS_GETRANDOM] = "getrandom",
    [SYS_CPUINFO] = "cpuinfo",
    [SYS_THREAD] = "thread",
    [SYS_TCP_LISTEN] = "tcp_listen",
    [SYS_TCP_ACCEPT] = "tcp_accept",
    [SYS_SCREEN_INFO] = "screen_info",
    [SYS_SCREEN_MAP] = "screen_map",
    [SYS_SCREEN_FLUSH] = "screen_flush",
    [SYS_INPUT_READ] = "input_read",
    [SYS_SCREEN_CURSOR] = "screen_cursor",
    [SYS_PTY_OPEN] = "pty_open",
    [SYS_PTY_SPAWN] = "pty_spawn",
    [SYS_PTY_READ] = "pty_read",
    [SYS_PTY_WRITE] = "pty_write",
};

const char *syscall_name(uint64_t number)
{
    return number < SYS_COUNT ? syscall_names[number] : "unknown";
}

/* A user pointer is never used directly: every page is looked up in the
   program's own page table (it must be a user page with the right
   permission) and copied through its physical address. A bad pointer
   becomes E_FAULT instead of a kernel crash. */
static int copy_from_user(void *destination, uintptr_t source, uint64_t length)
{
    uint8_t *to = (uint8_t *)destination;

    while (length > 0)
    {
        uintptr_t physical;

        if (vm_user_translate(process_user_root(), source, PTE_R, &physical) != 0)
        {
            return -1;
        }

        uint64_t chunk = PAGE_SIZE - (source & (PAGE_SIZE - 1));

        if (chunk > length)
        {
            chunk = length;
        }

        memcpy(to, (const void *)physical, chunk);
        to += chunk;
        source += chunk;
        length -= chunk;
    }

    return 0;
}

static int copy_to_user(uintptr_t destination, const void *source, uint64_t length)
{
    const uint8_t *from = (const uint8_t *)source;

    while (length > 0)
    {
        uintptr_t physical;

        if (vm_user_translate(process_user_root(), destination, PTE_W, &physical) != 0)
        {
            return -1;
        }

        uint64_t chunk = PAGE_SIZE - (destination & (PAGE_SIZE - 1));

        if (chunk > length)
        {
            chunk = length;
        }

        memcpy((void *)physical, from, chunk);
        from += chunk;
        destination += chunk;
        length -= chunk;
    }

    return 0;
}

static int copy_string_from_user(char *destination, uintptr_t source, uint64_t max)
{
    for (uint64_t i = 0; i < max; i++)
    {
        if (copy_from_user(&destination[i], source + i, 1) != 0)
        {
            return -1;
        }

        if (destination[i] == 0)
        {
            return 0;
        }
    }

    return -1;
}

static int64_t user_path(char *out, uintptr_t address)
{
    char raw[PATH_MAX];

    if (copy_string_from_user(raw, address, PATH_MAX) != 0)
    {
        return E_FAULT;
    }

    return process_resolve_path(raw, out) == 0 ? 0 : E_INVAL;
}

#define NET_CHUNK 4096

static int allowed(uint64_t number, uint32_t capability);

static int64_t sys_net(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2)
{
    if (!allowed(number, CAP_NET))
    {
        return E_PERM;
    }

    if (number == SYS_NET_INFO)
    {
        net_info_t info;

        net_info(&info);
        return copy_to_user(a0, &info, sizeof(info)) == 0 ? 0 : E_FAULT;
    }

    if (number == SYS_NET_RESOLVE)
    {
        char name[PATH_MAX];
        uint32_t address;

        if (copy_string_from_user(name, a0, PATH_MAX) != 0)
        {
            return E_FAULT;
        }

        int64_t result = net_resolve(name, &address);

        if (result != 0)
        {
            return result;
        }

        return copy_to_user(a1, &address, sizeof(address)) == 0 ? 0 : E_FAULT;
    }

    if (number == SYS_NET_PING)
    {
        return net_ping((uint32_t)a0, (uint32_t)a1);
    }

    if (number == SYS_TCP_CONNECT)
    {
        return net_connect((uint32_t)a0, (uint32_t)a1);
    }

    if (number == SYS_TCP_CLOSE)
    {
        return net_close((int)a0);
    }

    if (number == SYS_TCP_LISTEN)
    {
        return net_listen((uint32_t)a0);
    }

    if (number == SYS_TCP_ACCEPT)
    {
        uint32_t remote = 0;
        int64_t handle = net_accept((int)a0, &remote, a2);

        if (handle > 0 && a1 != 0 && copy_to_user(a1, &remote, sizeof(remote)) != 0)
        {
            net_close((int)handle);
            return E_FAULT;
        }

        return handle;
    }

    uint64_t length = a2 < NET_CHUNK ? a2 : NET_CHUNK;
    uint8_t *chunk = kmalloc(NET_CHUNK);
    int64_t result;

    if (!chunk)
    {
        return E_NOMEM;
    }

    if (number == SYS_TCP_SEND)
    {
        result = copy_from_user(chunk, a1, length) != 0 ? E_FAULT : net_send((int)a0, chunk, length);
    }
    else
    {
        result = net_recv((int)a0, chunk, length);

        if (result > 0 && copy_to_user(a1, chunk, (uint64_t)result) != 0)
        {
            result = E_FAULT;
        }
    }

    kfree(chunk);
    return result;
}

static int64_t sys_chdir(uintptr_t address)
{
    char path[PATH_MAX];
    uint32_t inode;
    knocfs_stat_t stat;
    int64_t result = user_path(path, address);

    if (result != 0)
    {
        return result;
    }

    if (knocfs_lookup(path, &inode) != 0 || knocfs_stat(inode, &stat) != 0)
    {
        return E_NOTFOUND;
    }

    if (stat.type != KNOCFS_TYPE_DIR)
    {
        return E_NOTDIR;
    }

    return process_chdir(path);
}

static int64_t sys_getcwd(uintptr_t buffer, uint64_t length)
{
    const char *cwd = process_cwd();
    uint64_t size = 0;

    while (cwd[size])
    {
        size++;
    }

    if (size + 1 > length)
    {
        return E_INVAL;
    }

    return copy_to_user(buffer, cwd, size + 1) == 0 ? (int64_t)size : E_FAULT;
}

static const char *capability_name(uint32_t capability)
{
    if (capability == CAP_SCREEN)
    {
        return "SCREEN";
    }

    if (capability == CAP_NET)
    {
        return "NET";
    }

    if (capability == CAP_CONSOLE)
    {
        return "CONSOLE";
    }

    if (capability == CAP_SPAWN)
    {
        return "SPAWN";
    }

    if (capability == CAP_FILES_READ)
    {
        return "FILES_READ";
    }

    if (capability == CAP_FILES_WRITE)
    {
        return "FILES_WRITE";
    }

    if (capability == CAP_SYSTEM)
    {
        return "SYSTEM";
    }

    if (capability == CAP_KNOWLEDGE)
    {
        return "KNOWLEDGE";
    }

    return "MEMORY";
}

static int allowed(uint64_t number, uint32_t capability)
{
    if (process_capabilities() & capability)
    {
        return 1;
    }

    process_note_denied();

    uart_puts("[SECURITY] ");
    uart_puts(process_current_name());
    uart_puts(" (pid ");
    uart_put_uint((uint64_t)process_current_pid());
    uart_puts(") called ");
    uart_puts(syscall_name(number));
    uart_puts(" without the ");
    uart_puts(capability_name(capability));
    uart_puts(" capability: denied\n");

    memgraph_record(GRAPH_KIND_PROGRAM, process_current_name(), GRAPH_REL_DENIED,
                    GRAPH_KIND_CAPABILITY, capability_name(capability), "security", 100);

    return 0;
}

#define CONSOLE_LINE_TICKS 5

static int console_line_owner;
static uint64_t console_line_since;

static void console_claim_line(void)
{
    int me = process_current_pid();

    while (console_line_owner != 0 && console_line_owner != me &&
           timer_ticks() - console_line_since < CONSOLE_LINE_TICKS && process_can_block())
    {
        process_sleep(1);
    }

    console_line_owner = me;
    console_line_since = timer_ticks();
}

static void console_finish_line(const char *data, uint64_t size)
{
    if (size > 0 && data[size - 1] == '\n')
    {
        console_line_owner = 0;
    }
}

static int64_t console_write(uintptr_t buffer, uint64_t length)
{
    char chunk[SYSCALL_CHUNK];
    device_t *console = device_find("uart0");
    uint64_t written = 0;

    if (length > SYSCALL_WRITE_MAX)
    {
        return E_INVAL;
    }

    while (written < length)
    {
        uint64_t size = length - written;

        if (size > SYSCALL_CHUNK)
        {
            size = SYSCALL_CHUNK;
        }

        if (copy_from_user(chunk, buffer + written, size) != 0)
        {
            return written > 0 ? (int64_t)written : E_FAULT;
        }

        if (process_pty())
        {
            if (!process_capture(chunk, size))
            {
                pty_output(process_pty(), chunk, size);
            }
        }
        else if (!process_capture(chunk, size))
        {
            console_claim_line();
            device_write(console, chunk, size);
            console_finish_line(chunk, size);
        }
        written += size;
    }

    return (int64_t)written;
}

static int64_t console_read(uintptr_t buffer, uint64_t length)
{
    char chunk[SYSCALL_CHUNK];

    if (length > SYSCALL_CHUNK)
    {
        length = SYSCALL_CHUNK;
    }

    /* Keys come through the terminal: the console process passes on the
       ones it doesn't handle itself. Blocks until one arrives. */
    int64_t count = tty_read(chunk, length);

    if (copy_to_user(buffer, chunk, (uint64_t)count) != 0)
    {
        return E_FAULT;
    }

    return count;
}

/* File data goes straight between the disk and the program's own pages:
   each page is checked in its page table and used through its physical
   address, so big reads (model weights) need no extra copy */
static int64_t file_io(open_file_t *file, uintptr_t buffer, uint64_t length, int writing)
{
    uint64_t done = 0;

    if (length > SYSCALL_FILE_IO_MAX)
    {
        length = SYSCALL_FILE_IO_MAX;
    }

    while (done < length)
    {
        uintptr_t address = buffer + done;
        uintptr_t physical;
        uint64_t chunk = PAGE_SIZE - (address & (PAGE_SIZE - 1));

        if (chunk > length - done)
        {
            chunk = length - done;
        }

        if (vm_user_translate(process_user_root(), address, writing ? PTE_R : PTE_W, &physical) != 0)
        {
            return done > 0 ? (int64_t)done : E_FAULT;
        }

        while (chunk < length - done && chunk < FILE_RUN_MAX)
        {
            uintptr_t next;
            uint64_t more = length - done - chunk < PAGE_SIZE ? length - done - chunk : PAGE_SIZE;

            if (vm_user_translate(process_user_root(), address + chunk, writing ? PTE_R : PTE_W, &next) != 0 ||
                next != physical + chunk)
            {
                break;
            }

            chunk += more;
        }

        int64_t result = writing
                             ? knocfs_write(file->inode, file->offset, (const void *)physical, chunk)
                             : knocfs_read(file->inode, file->offset, (void *)physical, chunk);

        if (result < 0)
        {
            return done > 0 ? (int64_t)done : result;
        }

        file->offset += (uint64_t)result;
        done += (uint64_t)result;
        process_note_disk((uint64_t)result);

        if ((uint64_t)result < chunk)
        {
            break;
        }
    }

    return (int64_t)done;
}

static int64_t sys_write(uint64_t fd, uintptr_t buffer, uint64_t length)
{
    if (fd == FD_STDOUT || fd == FD_STDERR)
    {
        if (!allowed(SYS_WRITE, CAP_CONSOLE))
        {
            return E_PERM;
        }

        return console_write(buffer, length);
    }

    open_file_t *file = process_file((int)fd);

    if (file == 0 || !(file->flags & O_WRITE))
    {
        return E_BADF;
    }

    return file_io(file, buffer, length, 1);
}

static int64_t sys_read(uint64_t fd, uintptr_t buffer, uint64_t length)
{
    if (fd == FD_STDIN)
    {
        if (!allowed(SYS_READ, CAP_CONSOLE))
        {
            return E_PERM;
        }

        return console_read(buffer, length);
    }

    open_file_t *file = process_file((int)fd);

    if (file == 0 || !(file->flags & O_READ))
    {
        return E_BADF;
    }

    return file_io(file, buffer, length, 0);
}

int64_t kfile_open(const char *path, uint64_t flags)
{
    uint32_t inode;
    knocfs_stat_t stat;

    if (flags & (O_WRITE | O_CREATE | O_TRUNC))
    {
        if (!allowed(SYS_OPEN, CAP_FILES_WRITE))
        {
            return E_PERM;
        }
    }
    else if (!allowed(SYS_OPEN, CAP_FILES_READ))
    {
        return E_PERM;
    }

    int result = knocfs_lookup(path, &inode);

    if (result == E_NOTFOUND && (flags & O_CREATE))
    {
        result = knocfs_create(path, KNOCFS_TYPE_FILE, &inode);
    }

    if (result != 0)
    {
        return result;
    }

    if (knocfs_stat(inode, &stat) != 0)
    {
        return E_IO;
    }

    if (stat.type == KNOCFS_TYPE_DIR && (flags & (O_WRITE | O_TRUNC)))
    {
        return E_ISDIR;
    }

    if ((flags & O_TRUNC) && knocfs_truncate(inode) != 0)
    {
        return E_IO;
    }

    int fd = process_file_open(inode, (uint32_t)flags);

    return fd < 0 ? E_NOSPACE : fd;
}

static int64_t sys_open(uintptr_t path_address, uint64_t flags)
{
    char path[PATH_MAX];
    int64_t path_result = user_path(path, path_address);

    return path_result != 0 ? path_result : kfile_open(path, flags);
}

static int64_t sys_close(uint64_t fd)
{
    open_file_t *file = process_file((int)fd);

    if (file == 0)
    {
        return E_BADF;
    }

    file->used = 0;
    return 0;
}

static int64_t sys_seek(uint64_t fd, uint64_t offset)
{
    open_file_t *file = process_file((int)fd);

    if (file == 0)
    {
        return E_BADF;
    }

    if (offset == SEEK_POSITION)
    {
        return (int64_t)file->offset;
    }

    if (offset == SEEK_SIZE)
    {
        knocfs_stat_t stat;

        return knocfs_stat(file->inode, &stat) == 0 ? (int64_t)stat.size : E_IO;
    }

    file->offset = offset;
    return (int64_t)offset;
}

static int64_t sys_stat(uintptr_t path_address, uintptr_t out)
{
    char path[PATH_MAX];
    uint32_t inode;
    knocfs_stat_t stat;
    file_stat_t result;

    if (!allowed(SYS_STAT, CAP_FILES_READ))
    {
        return E_PERM;
    }

    int64_t path_result = user_path(path, path_address);

    if (path_result != 0)
    {
        return path_result;
    }

    int error = knocfs_lookup(path, &inode);

    if (error == 0)
    {
        error = knocfs_stat(inode, &stat);
    }

    if (error != 0)
    {
        return error;
    }

    result.type = stat.type;
    result.extents = stat.extents;
    result.size = stat.size;
    result.created = stat.created;
    result.modified = stat.modified;

    return copy_to_user(out, &result, sizeof(result)) == 0 ? 0 : E_FAULT;
}

static int64_t sys_readdir(uintptr_t path_address, uint64_t index, uintptr_t out)
{
    char path[PATH_MAX];
    uint32_t directory;
    knocfs_dirent_t entry;
    knocfs_stat_t stat;
    dir_entry_t result;

    if (!allowed(SYS_READDIR, CAP_FILES_READ))
    {
        return E_PERM;
    }

    int64_t path_result = user_path(path, path_address);

    if (path_result != 0)
    {
        return path_result;
    }

    int error = knocfs_lookup(path, &directory);

    if (error == 0)
    {
        error = knocfs_readdir(directory, (uint32_t)index, &entry);
    }

    if (error == 0)
    {
        error = knocfs_stat(entry.inode, &stat);
    }

    if (error != 0)
    {
        return error;
    }

    memset(&result, 0, sizeof(result));
    memcpy(result.name, entry.name, FILE_NAME_MAX);
    result.type = stat.type;
    result.size = stat.size;
    result.modified = stat.modified;

    return copy_to_user(out, &result, sizeof(result)) == 0 ? 0 : E_FAULT;
}

static int64_t sys_wait(uint64_t pid)
{
    int exit_code = 0;

    /* Ctrl-C stops the program the shell is waiting for */
    int pty = process_pty();

    if (pty)
    {
        pty_set_foreground(pty, (int)pid);
    }
    else
    {
        tty_set_foreground((int)pid);
    }

    int result = process_wait((int)pid, &exit_code);

    if (pty)
    {
        pty_set_foreground(pty, 0);
    }
    else
    {
        tty_set_foreground(0);
    }

    if (result == -1)
    {
        return E_NOTFOUND;
    }

    if (result == -2)
    {
        return E_CRASHED;
    }

    return exit_code;
}

static int64_t sys_ps(uint64_t index, uintptr_t out)
{
    process_info_t info;

    if (process_info((uint32_t)index, &info) != 0)
    {
        return E_NOTFOUND;
    }

    return copy_to_user(out, &info, sizeof(info)) == 0 ? 0 : E_FAULT;
}

static int64_t sys_sysinfo(uintptr_t out)
{
    system_info_t info;
    uint32_t files = 0;

    memset(&info, 0, sizeof(info));

    info.ram_bytes = (uint64_t)page_total() * PAGE_SIZE;
    info.ram_free_bytes = (uint64_t)page_free_count() * PAGE_SIZE;
    info.uptime_ticks = timer_ticks();
    info.cpu_count = 2;

    knocfs_usage(&info.disk_bytes, &info.disk_free_bytes, &files);
    info.disk_files = files;

    guardian_system_info(&info);

    return copy_to_user(out, &info, sizeof(info)) == 0 ? 0 : E_FAULT;
}

static int64_t sys_devinfo(uint64_t index, uintptr_t out)
{
    device_t *dev = device_at((uint32_t)index);
    device_info_t info;

    if (dev == 0)
    {
        return E_NOTFOUND;
    }

    memset(&info, 0, sizeof(info));

    for (int i = 0; i < INFO_NAME_MAX - 1 && dev->name[i]; i++)
    {
        info.name[i] = dev->name[i];
    }

    info.irq = dev->irq;
    info.ready = (uint32_t)dev->ready;
    info.disabled = (uint32_t)dev->disabled;
    info.blocks = dev->block_count;

    return copy_to_user(out, &info, sizeof(info)) == 0 ? 0 : E_FAULT;
}

static int64_t sys_crashinfo(uint64_t index, uintptr_t out)
{
    crash_info_t info;

    if (guardian_crash_info((uint32_t)index, &info) != 0)
    {
        return E_NOTFOUND;
    }

    return copy_to_user(out, &info, sizeof(info)) == 0 ? 0 : E_FAULT;
}

static int64_t sys_graph(uintptr_t request_address, uintptr_t out)
{
    graph_request_t request;

    if (!allowed(SYS_GRAPH, CAP_KNOWLEDGE))
    {
        return E_PERM;
    }

    if (copy_from_user(&request, request_address, sizeof(request)) != 0)
    {
        return E_FAULT;
    }

    request.a[GRAPH_NAME_MAX - 1] = 0;
    request.b[GRAPH_NAME_MAX - 1] = 0;

    if (request.op == GRAPH_OP_RECORD)
    {
        return memgraph_record(request.kind_a, request.a, request.relation, request.kind_b,
                               request.b, process_current_name(), request.confidence);
    }

    if (request.op == GRAPH_OP_STATS)
    {
        graph_stats_t stats;
        int result = memgraph_stats(&stats);

        return result != 0 ? result : copy_to_user(out, &stats, sizeof(stats)) == 0 ? 0 : E_FAULT;
    }

    if (request.op == GRAPH_OP_RECENT || request.op == GRAPH_OP_EDGES)
    {
        graph_edge_info_t info;
        int result = request.op == GRAPH_OP_RECENT
                         ? memgraph_recent(request.index, &info)
                         : memgraph_edges(request.kind_a, request.a, request.index, &info);

        return result != 0 ? result : copy_to_user(out, &info, sizeof(info)) == 0 ? 0 : E_FAULT;
    }

    if (request.op == GRAPH_OP_FIND)
    {
        graph_node_info_t info;
        int result = memgraph_find(request.a, request.index, &info);

        return result != 0 ? result : copy_to_user(out, &info, sizeof(info)) == 0 ? 0 : E_FAULT;
    }

    if (request.op == GRAPH_OP_FORGET)
    {
        if (!allowed(SYS_GRAPH, CAP_SYSTEM))
        {
            return E_PERM;
        }

        return memgraph_forget(request.kind_a, request.a);
    }

    return E_INVAL;
}

int64_t kfile_path_change(uint64_t number, const char *path)
{
    uint32_t inode;

    if (!allowed(number, CAP_FILES_WRITE))
    {
        return E_PERM;
    }

    if (number == SYS_MKDIR)
    {
        return knocfs_create(path, KNOCFS_TYPE_DIR, &inode);
    }

    return knocfs_remove(path);
}

static int64_t sys_path_change(uint64_t number, uintptr_t path_address)
{
    char path[PATH_MAX];
    int64_t path_result = user_path(path, path_address);

    return path_result != 0 ? path_result : kfile_path_change(number, path);
}

static int64_t sys_spawn(uintptr_t name_address, uintptr_t args_address, int capture, int quiet)
{
    char name[PATH_MAX];
    char args[ARGS_MAX];

    if (copy_string_from_user(name, name_address, PATH_MAX) != 0)
    {
        return E_FAULT;
    }

    args[0] = 0;

    if (args_address != 0 && copy_string_from_user(args, args_address, ARGS_MAX) != 0)
    {
        return E_FAULT;
    }

    int by_path = 0;

    for (int i = 0; name[i]; i++)
    {
        by_path |= name[i] == '/';
    }

    const program_t *program = 0;

    if (by_path)
    {
        char path[PATH_MAX];

        if (process_resolve_path(name, path) != 0)
        {
            return E_INVAL;
        }

        program = program_at_path(path);
    }
    else
    {
        program = program_find(name);

        if (program == 0)
        {
            program = program_installed(name);
        }
    }

    if (program == 0)
    {
        return E_NOTFOUND;
    }

    int pid = capture ? process_spawn_capture(program, args, quiet) : process_spawn_args(program, args);

    return pid < 0 ? E_NOMEM : pid;
}

static int64_t sys_getargs(uintptr_t buffer, uint64_t length)
{
    const char *args = process_args();
    uint64_t size = 0;

    while (args[size])
    {
        size++;
    }

    if (length == 0)
    {
        return E_INVAL;
    }

    if (size >= length)
    {
        size = length - 1;
    }

    char terminator = 0;

    if (copy_to_user(buffer, args, size) != 0 || copy_to_user(buffer + size, &terminator, 1) != 0)
    {
        return E_FAULT;
    }

    return (int64_t)size;
}

int64_t kfile_rename(const char *from, const char *to)
{
    if (!allowed(SYS_RENAME, CAP_FILES_WRITE))
    {
        return E_PERM;
    }

    return knocfs_rename(from, to);
}

static int64_t sys_rename(uintptr_t from_address, uintptr_t to_address)
{
    char from[PATH_MAX];
    char to[PATH_MAX];

    if (!allowed(SYS_RENAME, CAP_FILES_WRITE))
    {
        return E_PERM;
    }

    int64_t path_result = user_path(from, from_address);

    if (path_result == 0)
    {
        path_result = user_path(to, to_address);
    }

    if (path_result != 0)
    {
        return path_result;
    }

    return knocfs_rename(from, to);
}

int user_copy_in(void *destination, uintptr_t source, uint64_t length)
{
    return copy_from_user(destination, source, length);
}

int user_copy_out(uintptr_t destination, const void *source, uint64_t length)
{
    return copy_to_user(destination, source, length);
}

int user_copy_string(char *destination, uintptr_t source, uint64_t max)
{
    return copy_string_from_user(destination, source, max);
}

int syscall_allowed(uint64_t number, uint32_t capability)
{
    return allowed(number, capability);
}

int64_t kfile_read(uint64_t fd, uintptr_t buffer, uint64_t length)
{
    return sys_read(fd, buffer, length);
}

int64_t kfile_write(uint64_t fd, uintptr_t buffer, uint64_t length)
{
    return sys_write(fd, buffer, length);
}

int64_t kfile_close(uint64_t fd)
{
    return sys_close(fd);
}

int64_t syscall_handle(trap_frame_t *frame)
{
    uint64_t number = frame->a7;

    if (process_is_linux())
    {
        return linux_syscall(frame);
    }

    process_record_syscall(number);

    switch (number)
    {
    case SYS_EXIT:
        process_exit_code((int)frame->a0);

    case SYS_WRITE:
        return sys_write(frame->a0, frame->a1, frame->a2);

    case SYS_READ:
        return sys_read(frame->a0, frame->a1, frame->a2);

    case SYS_GETPID:
        return process_current_pid();

    case SYS_YIELD:
        process_yield();
        return 0;

    case SYS_SLEEP:
        process_sleep(frame->a0 < SYSCALL_SLEEP_MAX ? frame->a0 : SYSCALL_SLEEP_MAX);
        return 0;

    case SYS_UPTIME:
        return (int64_t)timer_ticks();

    case SYS_TIME:
    {
        uint64_t seconds = rtc_seconds();

        return seconds ? (int64_t)seconds : E_NODEV;
    }

    case SYS_GETRANDOM:
    {
        uint8_t random[RANDOM_MAX];
        uint64_t length = frame->a1;

        if (length > RANDOM_MAX)
        {
            return E_INVAL;
        }

        if (virtio_rng_read(random, length) != (int64_t)length)
        {
            return E_NODEV;
        }

        return copy_to_user(frame->a0, random, length) == 0 ? (int64_t)length : E_FAULT;
    }

    case SYS_SPAWN:
        if (!allowed(number, CAP_SPAWN))
        {
            return E_PERM;
        }

        return sys_spawn(frame->a0, frame->a1, 0, 0);

    case SYS_SPAWN_CAPTURE:
        if (!allowed(number, CAP_SPAWN))
        {
            return E_PERM;
        }

        return sys_spawn(frame->a0, frame->a1, 1, frame->a2 != 0);

    case SYS_NET_INFO:
    case SYS_NET_RESOLVE:
    case SYS_NET_PING:
    case SYS_TCP_CONNECT:
    case SYS_TCP_SEND:
    case SYS_TCP_RECV:
    case SYS_TCP_CLOSE:
    case SYS_TCP_LISTEN:
    case SYS_TCP_ACCEPT:
        return sys_net(number, frame->a0, frame->a1, frame->a2);

    case SYS_CHDIR:
        return sys_chdir(frame->a0);

    case SYS_GETCWD:
        return sys_getcwd(frame->a0, frame->a1);

    case SYS_CAPTURED:
    {
        char chunk[PROCESS_CAPTURE_MAX];
        uint64_t length = process_captured(chunk, frame->a1 < sizeof(chunk) ? frame->a1 : sizeof(chunk));

        return copy_to_user(frame->a0, chunk, length) == 0 ? (int64_t)length : E_FAULT;
    }

    case SYS_MEM_ALLOC:
        if (!allowed(number, CAP_MEMORY))
        {
            return E_PERM;
        }

        return process_mem_alloc(frame->a0);

    case SYS_OPEN:
        return sys_open(frame->a0, frame->a1);

    case SYS_CLOSE:
        return sys_close(frame->a0);

    case SYS_SEEK:
        return sys_seek(frame->a0, frame->a1);

    case SYS_STAT:
        return sys_stat(frame->a0, frame->a1);

    case SYS_READDIR:
        return sys_readdir(frame->a0, frame->a1, frame->a2);

    case SYS_MKDIR:
    case SYS_REMOVE:
        return sys_path_change(number, frame->a0);

    case SYS_WAIT:
        return sys_wait(frame->a0);

    case SYS_GETARGS:
        return sys_getargs(frame->a0, frame->a1);

    case SYS_RENAME:
        return sys_rename(frame->a0, frame->a1);

    case SYS_GRAPH:
        return sys_graph(frame->a0, frame->a1);

    case SYS_TELEMETRY:
    {
        telemetry_sample_t sample;

        if (!allowed(SYS_TELEMETRY, CAP_SYSTEM))
        {
            return E_PERM;
        }

        if (telemetry_get((uint32_t)frame->a0, &sample) != 0)
        {
            return E_NOTFOUND;
        }

        return copy_to_user(frame->a1, &sample, sizeof(sample)) == 0 ? 0 : E_FAULT;
    }

    case SYS_SETCLASS:
        if (!allowed(SYS_SETCLASS, CAP_SYSTEM))
        {
            return E_PERM;
        }

        return process_lower_class_user((int)frame->a0, (uint32_t)frame->a1);

    case SYS_THREAD:
        return process_thread_spawn(frame->a0, frame->a1, frame->a2);

    case SYS_SCREEN_INFO:
    {
        screen_info_t info;
        int result = screen_info(&info);

        if (result != 0)
        {
            return result;
        }

        return copy_to_user(frame->a0, &info, sizeof(info)) == 0 ? 0 : E_FAULT;
    }

    case SYS_SCREEN_MAP:
        if (!allowed(number, CAP_SCREEN))
        {
            return E_PERM;
        }

        return screen_map();

    case SYS_SCREEN_FLUSH:
        return screen_flush((uint32_t)frame->a0, (uint32_t)frame->a1, (uint32_t)(frame->a2 >> 32),
                            (uint32_t)frame->a2);

    case SYS_INPUT_READ:
    {
        input_event_t events[32];
        uint32_t max = frame->a1 < 32 ? (uint32_t)frame->a1 : 32;
        int64_t count = screen_input(events, max, frame->a2 < 1000 ? frame->a2 : 1000);

        if (count <= 0)
        {
            return count;
        }

        return copy_to_user(frame->a0, events, (uint64_t)count * sizeof(input_event_t)) == 0 ? count : E_FAULT;
    }

    case SYS_SCREEN_CURSOR:
    {
        static uint32_t pixels[GPU_CURSOR_SIZE * GPU_CURSOR_SIZE];

        if (copy_from_user(pixels, frame->a0, sizeof(pixels)) != 0)
        {
            return E_FAULT;
        }

        return screen_cursor(pixels, (uint32_t)(frame->a1 >> 16), (uint32_t)(frame->a1 & 0xFFFF));
    }

    case SYS_PTY_OPEN:
        if (!allowed(number, CAP_SPAWN))
        {
            return E_PERM;
        }

        return pty_open(process_owner_pid());

    case SYS_PTY_SPAWN:
    {
        if (!allowed(number, CAP_SPAWN))
        {
            return E_PERM;
        }

        if (!pty_owned((int)frame->a0, process_owner_pid()))
        {
            return E_BADF;
        }

        process_set_spawn_pty((int)frame->a0);
        int64_t pid = sys_spawn(frame->a1, frame->a2, 0, 0);
        process_set_spawn_pty(0);
        return pid;
    }

    case SYS_PTY_READ:
    case SYS_PTY_WRITE:
    {
        char chunk[SYSCALL_CHUNK];
        uint64_t length = frame->a2 < sizeof(chunk) ? frame->a2 : sizeof(chunk);

        if (!pty_owned((int)frame->a0, process_owner_pid()))
        {
            return E_BADF;
        }

        if (number == SYS_PTY_READ)
        {
            int64_t count = pty_take_output((int)frame->a0, chunk, length);

            if (count <= 0)
            {
                return count;
            }

            return copy_to_user(frame->a1, chunk, (uint64_t)count) == 0 ? count : E_FAULT;
        }

        if (copy_from_user(chunk, frame->a1, length) != 0)
        {
            return E_FAULT;
        }

        return pty_give_input((int)frame->a0, chunk, length);
    }

    case SYS_CPUINFO:
    {
        cpu_info_t info;

        if (process_cpu_info((uint32_t)frame->a0, &info) != 0)
        {
            return E_NOTFOUND;
        }

        return copy_to_user(frame->a1, &info, sizeof(info)) == 0 ? 0 : E_FAULT;
    }

    case SYS_PS:
    case SYS_KILL:
    case SYS_SYSINFO:
    case SYS_DEVINFO:
    case SYS_CRASHINFO:
        if (!allowed(number, CAP_SYSTEM))
        {
            return E_PERM;
        }

        if (number == SYS_PS)
        {
            return sys_ps(frame->a0, frame->a1);
        }

        if (number == SYS_KILL)
        {
            return process_kill_user((int)frame->a0);
        }

        if (number == SYS_SYSINFO)
        {
            return sys_sysinfo(frame->a0);
        }

        if (number == SYS_DEVINFO)
        {
            return sys_devinfo(frame->a0, frame->a1);
        }

        return sys_crashinfo(frame->a0, frame->a1);

    default:
        return E_BADCALL;
    }
}
