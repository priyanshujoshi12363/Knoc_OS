#define AT_FDCWD -100

typedef unsigned long u64;

static long call(long n, long a, long b, long c, long d, long e, long f)
{
    register long a0 asm("a0") = a;
    register long a1 asm("a1") = b;
    register long a2 asm("a2") = c;
    register long a3 asm("a3") = d;
    register long a4 asm("a4") = e;
    register long a5 asm("a5") = f;
    register long a7 asm("a7") = n;

    asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a7) : "memory");
    return a0;
}

static u64 length(const char *s)
{
    u64 n = 0;

    while (s[n])
    {
        n++;
    }

    return n;
}

static void out(const char *s)
{
    call(64, 1, (long)s, (long)length(s), 0, 0, 0);
}

static void number(long v)
{
    char buffer[24];
    int i = 23;
    int negative = v < 0;
    u64 u = negative ? (u64)-v : (u64)v;

    buffer[i] = 0;

    do
    {
        buffer[--i] = (char)('0' + u % 10);
        u /= 10;
    } while (u);

    if (negative)
    {
        buffer[--i] = '-';
    }

    out(buffer + i);
}

static int same(const char *a, const char *b)
{
    while (*a && *a == *b)
    {
        a++;
        b++;
    }

    return *a == *b;
}

static int starts(const char *a, const char *b)
{
    while (*b && *a == *b)
    {
        a++;
        b++;
    }

    return *b == 0;
}

static void line(const char *a, long v, const char *b)
{
    out("linuxtest: ");
    out(a);

    if (v != -999999)
    {
        number(v);
    }

    out(b);
    out("\n");
}

static char utsname[6 * 65];
static char buffer[4096];
static char dents[4096];

int main(long argc, char **argv, char **envp)
{
    line("hello from a Linux program, argc ", argc, "");

    if (argc > 1 && same(argv[1], "crash"))
    {
        out("linuxtest: writing through a null pointer\n");
        volatile long *volatile bad = (volatile long *)8;

        *bad = 1;
    }

    if (argc > 1)
    {
        out("linuxtest: argv[1] = ");
        out(argv[1]);
        out("\n");
    }

    for (char **e = envp; *e; e++)
    {
        if (starts(*e, "PATH="))
        {
            out("linuxtest: environment has ");
            out(*e);
            out("\n");
        }
    }

    call(160, (long)utsname, 0, 0, 0, 0, 0);
    out("linuxtest: uname says ");
    out(utsname);
    out(" ");
    out(utsname + 4 * 65);
    out("\n");

    long fd = call(56, AT_FDCWD, (long)"/hello.txt", 0, 0, 0, 0);
    long got = call(63, fd, (long)buffer, sizeof(buffer), 0, 0, 0);

    call(57, fd, 0, 0, 0, 0, 0);
    line("read ", got, " bytes from /hello.txt");

    fd = call(56, AT_FDCWD, (long)"/tmp/lx.txt", 01 | 0100 | 01000, 0644, 0, 0);
    call(64, fd, (long)"written by linux\n", 17, 0, 0, 0);
    call(57, fd, 0, 0, 0, 0, 0);
    fd = call(56, AT_FDCWD, (long)"/tmp/lx.txt", 0, 0, 0, 0);
    got = call(63, fd, (long)buffer, sizeof(buffer), 0, 0, 0);

    long end = call(62, fd, 0, 2, 0, 0, 0);

    call(57, fd, 0, 0, 0, 0, 0);

    if (got == 17 && end == 17 && buffer[0] == 'w')
    {
        line("write, read back and lseek ok", -999999, "");
    }

    long st[16];

    call(79, AT_FDCWD, (long)"/tmp/lx.txt", (long)st, 0, 0, 0);
    line("stat size ", st[6], (((unsigned)st[2]) & 0170000) == 0100000 ? ", a regular file" : ", WRONG type");
    line("missing file gives ", call(56, AT_FDCWD, (long)"/nope/none", 0, 0, 0, 0), " (ENOENT is -2)");

    long base = call(214, 0, 0, 0, 0, 0, 0);
    long grown = call(214, base + 100000, 0, 0, 0, 0, 0);
    char *heap = (char *)base;

    heap[99999] = 42;

    if (grown == base + 100000 && heap[99999] == 42)
    {
        line("brk grew the heap by 100000 bytes", -999999, "");
    }

    char *map = (char *)call(222, 0, 1 << 20, 3, 0x22, -1, 0);

    map[(1 << 20) - 1] = 7;

    if ((long)map > 0 && map[(1 << 20) - 1] == 7 && map[0] == 0)
    {
        line("mmap gave 1048576 zeroed bytes", -999999, "");
    }

    long ts[2];

    call(113, 0, (long)ts, 0, 0, 0, 0);
    line("the clock says year ", 1970 + ts[0] / 31556952, "");

    unsigned char random[16];
    int nonzero = 0;

    call(278, (long)random, 16, 0, 0, 0, 0);

    for (int i = 0; i < 16; i++)
    {
        nonzero |= random[i];
    }

    if (nonzero)
    {
        line("getrandom gave 16 random bytes", -999999, "");
    }

    fd = call(56, AT_FDCWD, (long)"/", 0200000, 0, 0, 0);
    got = call(61, fd, (long)dents, sizeof(dents), 0, 0, 0);
    call(57, fd, 0, 0, 0, 0, 0);

    for (long p = 0; p < got;)
    {
        unsigned short reclen = *(unsigned short *)(dents + p + 16);

        if (same(dents + p + 19, "bin"))
        {
            line("getdents64 found /bin", -999999, "");
        }

        p += reclen;
    }

    call(34, AT_FDCWD, (long)"/tmp/lxdir", 0755, 0, 0, 0);

    long made = call(79, AT_FDCWD, (long)"/tmp/lxdir", (long)st, 0, 0, 0);
    long removed = call(35, AT_FDCWD, (long)"/tmp/lxdir", 0x200, 0, 0, 0);

    if (made == 0 && removed == 0)
    {
        line("mkdirat and unlinkat ok", -999999, "");
    }

    line("an unknown system call gives ", call(999, 0, 0, 0, 0, 0, 0), " (ENOSYS is -38)");
    return 3;
}

__attribute__((section(".text.start"), naked)) void _start(void)
{
    asm volatile("ld a0, 0(sp)\n"
                 "addi a1, sp, 8\n"
                 "slli a2, a0, 3\n"
                 "add a2, a1, a2\n"
                 "addi a2, a2, 8\n"
                 "call main\n"
                 "li a7, 94\n"
                 "ecall\n");
}
