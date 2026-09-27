#include "ulib.h"
#include "assist.h"

#define LINE_MAX 400
#define TRANSCRIPT_MAX 16384

static char line[LINE_MAX];
static char transcript[TRANSCRIPT_MAX];
static unsigned long transcript_length;

static void read_line(void)
{
    unsigned long length = 0;

    while (1)
    {
        char c;

        if (read(FD_STDIN, &c, 1) <= 0)
        {
            exit(0);
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

        if ((unsigned char)c < ' ' || length >= LINE_MAX - 1)
        {
            continue;
        }

        line[length++] = c;
        write(FD_STDOUT, &c, 1);
    }
}

static void remember(const char *who, const char *text)
{
    const char *parts[3] = {who, text, "\n"};

    for (int p = 0; p < 3; p++)
    {
        for (const char *s = parts[p]; *s && transcript_length < TRANSCRIPT_MAX - 1; s++)
        {
            transcript[transcript_length++] = *s;
        }
    }

    transcript[transcript_length] = 0;
}

static int starts_with(const char *text, const char *prefix)
{
    while (*prefix && *text == *prefix)
    {
        text++;
        prefix++;
    }

    return *prefix == 0;
}

static void save(const char *path)
{
    if (path[0] != '/')
    {
        print("chat: use /save /home/NAME.txt\n");
        return;
    }

    int fd = open(path, O_WRITE | O_CREATE | O_TRUNC);

    if (fd < 0)
    {
        print("chat: cannot write ");
        print(path);
        print("\n");
        return;
    }

    write(fd, transcript, transcript_length);
    close(fd);
    print("chat: saved ");
    print_uint(transcript_length);
    print(" bytes to ");
    print(path);
    print("\n");
}

static void help(void)
{
    print("Talk to KnocOS. It remembers the conversation, reads the memory graph when it helps,\n");
    print("and can use the agent's tools (it asks y/n before it changes anything).\n");
    print("  /new          start a new conversation\n");
    print("  /save FILE    save the conversation to a file\n");
    print("  /tools        list the tools and apps it can use\n");
    print("  /exit         leave the chat\n");
}

int main(void)
{
    int started = 0;
    int loaded = 0;

    assist_init("knocos: ");
    print("KnocOS chat. Type a message, /help for commands, /exit to leave.\n");

    while (1)
    {
        print("you: ");
        read_line();

        if (line[0] == 0)
        {
            continue;
        }

        if (strcmp(line, "/exit") == 0 || strcmp(line, "/quit") == 0)
        {
            print("chat: bye\n");
            return 0;
        }

        if (strcmp(line, "/help") == 0)
        {
            help();
            continue;
        }

        if (strcmp(line, "/tools") == 0)
        {
            assist_list_tools();
            continue;
        }

        if (strcmp(line, "/new") == 0)
        {
            started = 0;
            transcript_length = 0;
            transcript[0] = 0;
            print("chat: new conversation\n");
            continue;
        }

        if (starts_with(line, "/save"))
        {
            const char *path = line + 5;

            while (*path == ' ')
            {
                path++;
            }

            save(path);
            continue;
        }

        if (line[0] == '/')
        {
            print("chat: unknown command, type /help\n");
            continue;
        }

        if (!loaded)
        {
            print("chat: loading the language model...\n");

            if (assist_load_model() != 0)
            {
                continue;
            }

            loaded = 1;
        }

        unsigned long began = uptime();

        remember("you: ", line);

        int continued = started ? assist_continue(line, 1) : 0;

        if (continued == -1)
        {
            print("chat: the conversation is full, starting a new one\n");
        }

        if (continued != 0)
        {
            started = 0;
        }

        if (!started)
        {
            assist_begin(line, 1);
            started = 1;
        }

        int result = assist_reply();

        remember("knocos: ", assist_last_reply());

        if (result < 0)
        {
            print("chat: the conversation is full; the next message starts a new one\n");
            started = 0;
        }
        else if (result > 0)
        {
            print("chat: stopped after too many tool steps\n");
        }

        print("(");
        print_uint((uptime() - began) / 100);
        print(" s)\n");
    }
}
