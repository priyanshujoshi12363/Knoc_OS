#include "ulib.h"

/* Files survive reboots: the note written on one boot is read on the next */

#define NOTE_PATH "/home/note.txt"
#define NOTE_TEXT "KnocOS remembers this"
#define BIG_PATH "/tmp/big.bin"
#define BIG_SIZE 20000

static unsigned char big[BIG_SIZE];

static unsigned long length_of(const char *text)
{
    unsigned long length = 0;

    while (text[length])
    {
        length++;
    }

    return length;
}

static int check(int ok, const char *what)
{
    if (!ok)
    {
        print("[files] failed: ");
        print(what);
        print("\n");
    }

    return ok;
}

int main(void)
{
    char text[64];
    int fd = open(NOTE_PATH, O_READ);

    if (fd >= 0)
    {
        long count = read(fd, text, sizeof(text) - 1);

        text[count > 0 ? count : 0] = 0;
        close(fd);

        print("[files] found my note from the last boot: ");
        print(text);
        print("\n");
    }
    else
    {
        print("[files] no note yet, writing " NOTE_PATH "\n");
    }

    fd = open(NOTE_PATH, O_WRITE | O_CREATE | O_TRUNC);

    if (!check(fd >= 0, "create note") ||
        !check(write(fd, NOTE_TEXT, length_of(NOTE_TEXT)) == (long)length_of(NOTE_TEXT), "write note"))
    {
        return 1;
    }

    close(fd);

    fd = open("/hello.txt", O_READ);

    if (!check(fd >= 0, "open /hello.txt"))
    {
        return 2;
    }

    long count = read(fd, text, sizeof(text) - 1);

    text[count > 0 ? count : 0] = 0;
    close(fd);

    print("[files] /hello.txt says: ");
    print(text);
    print("\n");

    dir_entry_t entry;
    unsigned long programs = 0;

    print("[files] /bin:");

    while (readdir("/bin", programs, &entry) == 0)
    {
        print(" ");
        print(entry.name);
        programs++;
    }

    print("\n");

    if (!check(programs >= 7, "list /bin"))
    {
        return 3;
    }

    for (unsigned long i = 0; i < BIG_SIZE; i++)
    {
        big[i] = (unsigned char)(i * 13 + 5);
    }

    fd = open(BIG_PATH, O_READ | O_WRITE | O_CREATE | O_TRUNC);

    if (!check(fd >= 0, "create big file") ||
        !check(write(fd, big, BIG_SIZE) == BIG_SIZE, "write big file"))
    {
        return 4;
    }

    memset(big, 0, BIG_SIZE);
    seek(fd, 0);

    if (!check(read(fd, big, BIG_SIZE) == BIG_SIZE, "read big file back"))
    {
        return 5;
    }

    close(fd);

    for (unsigned long i = 0; i < BIG_SIZE; i++)
    {
        if (!check(big[i] == (unsigned char)(i * 13 + 5), "big file contents"))
        {
            return 6;
        }
    }

    file_stat_t info;

    if (!check(remove(BIG_PATH) == 0, "remove big file") ||
        !check(stat(BIG_PATH, &info) == E_NOTFOUND, "big file is gone") ||
        !check(open("/no/such/file", O_READ) == E_NOTFOUND, "missing file") ||
        !check(mkdir("/home") == E_EXISTS, "mkdir existing"))
    {
        return 7;
    }

    print("[files] 20000 bytes written across 5 blocks, read back, removed\n");
    return 0;
}
