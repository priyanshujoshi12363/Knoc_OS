#include "ulib.h"
#include "llm.h"
#include "rag.h"

#define APPS_DIR "/etc/apps"
#define APPS_MAX 16
#define STEPS_MAX 8
#define CALLS_MAX 4
#define ARGS_COUNT 6
#define KEY_MAX 24
#define VALUE_MAX 512
#define OUTPUT_MAX 4096
#define OBSERVATION_MAX 700
#define STEP_TOKENS 300
#define GENERATED_MAX 2048
#define READ_LIMIT 1500
#define FIND_DEPTH 6
#define FIND_HITS 20
#define LIST_MAX 40
#define PREFIX_PATH "/tmp/agent-prefix.kv"
#define SYSTEM_TEXT                                                                                  \
    "You are the KnocOS agent inside the KnocOS operating system. Use the tools to do what the user " \
    "asks, then answer in one or two sentences. Paths start with /. The user's files are in /home, "   \
    "downloads in /home/Downloads."

typedef enum
{
    RISK_READ,
    RISK_CHANGE
} risk_t;

typedef struct arg
{
    char key[KEY_MAX];
    char value[VALUE_MAX];
} arg_t;

typedef struct call
{
    char name[32];
    int count;
    arg_t args[ARGS_COUNT];
} call_t;

typedef struct text
{
    char *data;
    int length;
    int room;
} text_t;

typedef struct tool
{
    const char *name;
    const char *description;
    const char *params;
    const char *writes;
    risk_t risk;
    int (*run)(const call_t *call, text_t *out);
} tool_t;

typedef struct app
{
    char name[16];
    char description[200];
    char usage[80];
    risk_t risk;
} app_t;

typedef struct guide
{
    char partial[40];
    int active;
    const char *choices[APPS_MAX + 20];
    int choice_count;
} guide_t;

static app_t apps[APPS_MAX];
static int app_count;
static char output[OUTPUT_MAX];
static char generated[GENERATED_MAX];
static char system_prompt[6144];
static llm_prompt_t prompt;
static rag_facts_t facts;
static guide_t guide;
static int llm_ready;

static void add(text_t *t, const char *s)
{
    while (*s && t->length < t->room - 1)
    {
        t->data[t->length++] = *s++;
    }

    t->data[t->length] = 0;
}

static void add_bytes(text_t *t, const char *s, int n)
{
    for (int i = 0; i < n && t->length < t->room - 1; i++)
    {
        t->data[t->length++] = s[i];
    }

    t->data[t->length] = 0;
}

static void add_uint(text_t *t, unsigned long v)
{
    char digits[24];
    int n = 0;

    do
    {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < 23);

    while (n > 0)
    {
        char c[2] = {digits[--n], 0};

        add(t, c);
    }
}

static void add_json_string(text_t *t, const char *s)
{
    add(t, "\"");

    for (; *s; s++)
    {
        if (*s == '"' || *s == '\\')
        {
            add(t, "\\");
            add_bytes(t, s, 1);
        }
        else if (*s == '\n')
        {
            add(t, "\\n");
        }
        else
        {
            add_bytes(t, s, 1);
        }
    }

    add(t, "\"");
}

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

static int contains(const char *text, const char *part)
{
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

static int starts_with(const char *text, const char *prefix)
{
    while (*prefix && *text == *prefix)
    {
        text++;
        prefix++;
    }

    return *prefix == 0;
}

static void copy_text(char *to, const char *from, int room)
{
    int i = 0;

    while (from[i] && i < room - 1)
    {
        to[i] = from[i];
        i++;
    }

    to[i] = 0;
}

static const char *arg(const call_t *call, const char *key)
{
    for (int i = 0; i < call->count; i++)
    {
        if (strcmp(call->args[i].key, key) == 0)
        {
            return call->args[i].value;
        }
    }

    return 0;
}

static void set_arg(call_t *call, const char *key, const char *value)
{
    if (call->count >= ARGS_COUNT)
    {
        return;
    }

    copy_text(call->args[call->count].key, key, KEY_MAX);
    copy_text(call->args[call->count].value, value, VALUE_MAX);
    call->count++;
}

static int good_path(const char *path)
{
    return path && path[0] == '/' && !contains(path, "..");
}

static int writable_path(const char *path)
{
    return good_path(path) && (starts_with(path, "/home/") || strcmp(path, "/home") == 0 ||
                               starts_with(path, "/tmp/") || strcmp(path, "/tmp") == 0);
}

static int refuse(text_t *out, const char *why)
{
    add(out, "error: ");
    add(out, why);
    return -1;
}

static int tool_list_folder(const call_t *call, text_t *out)
{
    const char *path = arg(call, "path");
    dir_entry_t entry;
    int shown = 0;

    if (!good_path(path))
    {
        return refuse(out, "path must start with /");
    }

    for (unsigned long i = 0; readdir(path, i, &entry) == 0; i++)
    {
        if (shown++ >= LIST_MAX)
        {
            add(out, "...\n");
            break;
        }

        add(out, entry.name);

        if (entry.type == FILE_TYPE_DIR)
        {
            add(out, "/\n");
        }
        else
        {
            add(out, " (");
            add_uint(out, entry.size);
            add(out, " bytes)\n");
        }
    }

    if (shown == 0)
    {
        add(out, "empty or not a folder");
    }

    return 0;
}

static int tool_read_file(const call_t *call, text_t *out)
{
    const char *path = arg(call, "path");
    static char data[READ_LIMIT + 1];

    if (!good_path(path))
    {
        return refuse(out, "path must start with /");
    }

    int fd = open(path, O_READ);

    if (fd < 0)
    {
        return refuse(out, "cannot open the file");
    }

    long got = read(fd, data, READ_LIMIT);
    int binary = 0;

    close(fd);

    for (long i = 0; i < got; i++)
    {
        unsigned char c = (unsigned char)data[i];

        binary += c < 9 || (c > 13 && c < 32);
    }

    if (got > 0 && binary * 10 > got)
    {
        add(out, "binary file, not text");
        return 0;
    }

    add_bytes(out, data, got > 0 ? (int)got : 0);
    return 0;
}

static void find_in(const char *folder, const char *name, int depth, text_t *out, int *hits)
{
    dir_entry_t entry;
    char path[160];

    for (unsigned long i = 0; *hits < FIND_HITS && readdir(folder, i, &entry) == 0; i++)
    {
        text_t p = {path, 0, sizeof(path)};

        add(&p, folder);

        if (strcmp(folder, "/") != 0)
        {
            add(&p, "/");
        }

        add(&p, entry.name);

        if (contains(entry.name, name))
        {
            add(out, path);
            add(out, entry.type == FILE_TYPE_DIR ? "/\n" : "\n");
            (*hits)++;
        }

        if (entry.type == FILE_TYPE_DIR && depth < FIND_DEPTH)
        {
            find_in(path, name, depth + 1, out, hits);
        }
    }
}

static int tool_find_files(const call_t *call, text_t *out)
{
    const char *name = arg(call, "name");
    const char *folder = arg(call, "folder");
    int hits = 0;

    if (!name || !name[0])
    {
        return refuse(out, "missing name");
    }

    find_in(good_path(folder) ? folder : "/home", name, 0, out, &hits);

    if (hits == 0)
    {
        add(out, "no files found");
    }

    return 0;
}

static void add_facts(const char *query, text_t *out)
{
    rag_collect(query, &facts);

    for (int i = 0; i < facts.count; i++)
    {
        add(out, facts.text[i]);
        add(out, "\n");
    }
}

static int tool_system_status(const call_t *call, text_t *out)
{
    process_info_t info;

    (void)call;
    add_facts("what is wrong slow crash", out);
    add(out, "Running programs:");

    for (unsigned long i = 0; ps(i, &info) == 0; i++)
    {
        if (info.user)
        {
            add(out, " ");
            add(out, info.name);
        }
    }

    add(out, "\n");
    return 0;
}

static int tool_memory_search(const call_t *call, text_t *out)
{
    const char *query = arg(call, "text");

    if (!query || !query[0])
    {
        return refuse(out, "missing text");
    }

    add_facts(query, out);

    if (facts.count == 0)
    {
        add(out, "nothing found in the memory graph");
    }

    return 0;
}

static int tool_write_file(const call_t *call, text_t *out)
{
    const char *path = arg(call, "path");
    const char *content = arg(call, "text");

    if (!writable_path(path))
    {
        return refuse(out, "the agent may only write inside /home and /tmp");
    }

    int fd = open(path, O_WRITE | O_CREATE | O_TRUNC);

    if (fd < 0)
    {
        return refuse(out, "cannot create the file");
    }

    long length = content ? (long)strlen(content) : 0;
    long written = length ? write(fd, content, (unsigned long)length) : 0;

    close(fd);

    if (written != length)
    {
        return refuse(out, "write failed");
    }

    add(out, "wrote ");
    add_uint(out, (unsigned long)length);
    add(out, " bytes to ");
    add(out, path);
    return 0;
}

static int tool_make_folder(const call_t *call, text_t *out)
{
    const char *path = arg(call, "path");

    if (!writable_path(path))
    {
        return refuse(out, "the agent may only create folders inside /home and /tmp");
    }

    if (mkdir(path) != 0)
    {
        return refuse(out, "cannot create the folder (it may exist already)");
    }

    add(out, "created ");
    add(out, path);
    return 0;
}

static int tool_move(const call_t *call, text_t *out)
{
    const char *from = arg(call, "from");
    const char *to = arg(call, "to");

    if (!writable_path(from) || !writable_path(to))
    {
        return refuse(out, "the agent may only move files inside /home and /tmp");
    }

    if (rename(from, to) != 0)
    {
        return refuse(out, "cannot move it");
    }

    add(out, "moved ");
    add(out, from);
    add(out, " to ");
    add(out, to);
    return 0;
}

static int tool_copy(const call_t *call, text_t *out)
{
    const char *from = arg(call, "from");
    const char *to = arg(call, "to");
    static char chunk[4096];

    if (!good_path(from) || !writable_path(to))
    {
        return refuse(out, "the agent may only copy into /home and /tmp");
    }

    int in = open(from, O_READ);
    int dest = in < 0 ? -1 : open(to, O_WRITE | O_CREATE | O_TRUNC);
    unsigned long total = 0;
    long got;

    if (in < 0 || dest < 0)
    {
        if (in >= 0)
        {
            close(in);
        }

        return refuse(out, "cannot open the files");
    }

    while ((got = read(in, chunk, sizeof(chunk))) > 0 && write(dest, chunk, (unsigned long)got) == got)
    {
        total += (unsigned long)got;
    }

    close(in);
    close(dest);
    add(out, "copied ");
    add_uint(out, total);
    add(out, " bytes to ");
    add(out, to);
    return 0;
}

static int protected_program(const char *name)
{
    return strcmp(name, "knocsh") == 0 || strcmp(name, "healthd") == 0 || strcmp(name, "agent") == 0;
}

static int find_user_program(const char *name, process_info_t *info)
{
    for (unsigned long i = 0; ps(i, info) == 0; i++)
    {
        if (info->user && strcmp(info->name, name) == 0)
        {
            return 1;
        }
    }

    return 0;
}

static int tool_stop_program(const call_t *call, text_t *out)
{
    const char *name = arg(call, "name");
    process_info_t info;

    if (!name || protected_program(name))
    {
        return refuse(out, "that program cannot be stopped by the agent");
    }

    if (!find_user_program(name, &info))
    {
        return refuse(out, "no running program with that name");
    }

    if (kill(info.pid) != 0)
    {
        return refuse(out, "could not stop it");
    }

    graph_record(GRAPH_KIND_ACTOR, "agent", GRAPH_REL_STOPPED, GRAPH_KIND_PROGRAM, name, 100);
    add(out, "stopped ");
    add(out, name);
    return 0;
}

static int tool_lower_priority(const call_t *call, text_t *out)
{
    const char *name = arg(call, "name");
    process_info_t info;

    if (!name || protected_program(name))
    {
        return refuse(out, "that program cannot be changed by the agent");
    }

    if (!find_user_program(name, &info))
    {
        return refuse(out, "no running program with that name");
    }

    if (setclass(info.pid, CLASS_BACKGROUND) != 0)
    {
        return refuse(out, "could not lower its priority");
    }

    graph_record(GRAPH_KIND_ACTOR, "agent", GRAPH_REL_LOWERED, GRAPH_KIND_PROGRAM, name, 100);
    add(out, "moved ");
    add(out, name);
    add(out, " to background priority");
    return 0;
}

static const app_t *find_app(const char *name)
{
    for (int i = 0; i < app_count; i++)
    {
        if (name && strcmp(apps[i].name, name) == 0)
        {
            return &apps[i];
        }
    }

    return 0;
}

static int tool_run_app(const call_t *call, text_t *out)
{
    const char *name = arg(call, "app");
    const char *args = arg(call, "args");
    static char captured_output[4096];

    if (!find_app(name))
    {
        return refuse(out, "no such app (apps are described in /etc/apps)");
    }

    int pid = spawn_capture(name, args ? args : "");

    if (pid < 0)
    {
        return refuse(out, "the app could not start");
    }

    long code = wait(pid);
    long length = captured(captured_output, sizeof(captured_output));

    add_bytes(out, captured_output, length > 0 ? (int)length : 0);
    add(out, "\n[exit code ");

    if (code < 0)
    {
        add(out, "-");
        code = -code;
    }

    add_uint(out, (unsigned long)code);
    add(out, "]");
    return 0;
}

static const tool_t tools[] = {
    {"list_folder", "List the files and folders in a folder", "path", "", RISK_READ, tool_list_folder},
    {"read_file", "Read a text file", "path", "", RISK_READ, tool_read_file},
    {"find_files", "Find files whose name contains a text, searching a folder and its subfolders", "name,folder", "",
     RISK_READ, tool_find_files},
    {"system_status", "Show CPU, memory, disk, recent problems, fixes, crashes and running programs", "", "",
     RISK_READ, tool_system_status},
    {"memory_search", "Search the memory graph: what happened to a file, program or driver", "text", "", RISK_READ,
     tool_memory_search},
    {"write_file", "Create or overwrite a text file", "path,text", "path", RISK_CHANGE, tool_write_file},
    {"make_folder", "Create a folder", "path", "path", RISK_CHANGE, tool_make_folder},
    {"move", "Move or rename a file or folder", "from,to", "from,to", RISK_CHANGE, tool_move},
    {"copy", "Copy a file", "from,to", "to", RISK_CHANGE, tool_copy},
    {"stop_program", "Stop a running program", "name", "", RISK_CHANGE, tool_stop_program},
    {"lower_priority", "Move a running program to background priority", "name", "", RISK_CHANGE,
     tool_lower_priority},
    {"run_app", "Run a KnocOS app and read its output", "app,args", "", RISK_CHANGE, tool_run_app},
};

#define TOOL_COUNT ((int)(sizeof(tools) / sizeof(tools[0])))

static const tool_t *find_tool(const char *name)
{
    for (int i = 0; i < TOOL_COUNT; i++)
    {
        if (strcmp(tools[i].name, name) == 0)
        {
            return &tools[i];
        }
    }

    return 0;
}

static int optional_param(const char *tool, const char *param)
{
    return (strcmp(tool, "find_files") == 0 && strcmp(param, "folder") == 0) ||
           (strcmp(tool, "run_app") == 0 && strcmp(param, "args") == 0) ||
           (strcmp(tool, "write_file") == 0 && strcmp(param, "text") == 0);
}

static void read_manifest(const char *path)
{
    static char data[512];
    int fd = open(path, O_READ);

    if (fd < 0 || app_count >= APPS_MAX)
    {
        return;
    }

    long got = read(fd, data, sizeof(data) - 1);

    close(fd);
    data[got > 0 ? got : 0] = 0;

    app_t *app = &apps[app_count];

    memset(app, 0, sizeof(*app));
    app->risk = RISK_CHANGE;

    for (char *line = data; *line;)
    {
        char *end = line;

        while (*end && *end != '\n')
        {
            end++;
        }

        char saved = *end;

        *end = 0;

        char *value = line;

        while (*value && *value != ':')
        {
            value++;
        }

        if (*value == ':')
        {
            *value++ = 0;

            while (*value == ' ')
            {
                value++;
            }

            if (strcmp(line, "name") == 0)
            {
                copy_text(app->name, value, sizeof(app->name));
            }
            else if (strcmp(line, "description") == 0)
            {
                copy_text(app->description, value, sizeof(app->description));
            }
            else if (strcmp(line, "usage") == 0)
            {
                copy_text(app->usage, value, sizeof(app->usage));
            }
            else if (strcmp(line, "risk") == 0)
            {
                app->risk = strcmp(value, "read") == 0 ? RISK_READ : RISK_CHANGE;
            }
        }

        line = saved ? end + 1 : end;
    }

    if (app->name[0])
    {
        app_count++;
    }
}

static void load_apps(void)
{
    dir_entry_t entry;
    char path[96];

    for (unsigned long i = 0; readdir(APPS_DIR, i, &entry) == 0; i++)
    {
        text_t p = {path, 0, sizeof(path)};

        add(&p, APPS_DIR "/");
        add(&p, entry.name);

        if (entry.type == FILE_TYPE_FILE && contains(entry.name, ".app"))
        {
            read_manifest(path);
        }
    }
}

static void add_tool_json(text_t *t, const tool_t *tool)
{
    add(t, "{\"type\": \"function\", \"function\": {\"name\": \"");
    add(t, tool->name);
    add(t, "\", \"description\": ");

    if (tool->run == tool_run_app)
    {
        char description[1024];
        text_t d = {description, 0, sizeof(description)};

        add(&d, tool->description);
        add(&d, ". Apps:");

        for (int i = 0; i < app_count; i++)
        {
            add(&d, " ");
            add(&d, apps[i].name);

            if (apps[i].usage[0])
            {
                add(&d, " ");
                add(&d, apps[i].usage);
            }

            add(&d, ": ");
            add(&d, apps[i].description);
        }

        add_json_string(t, description);
    }
    else
    {
        add_json_string(t, tool->description);
    }

    add(t, ", \"parameters\": {\"type\": \"object\", \"properties\": {");

    char params[64];
    int first = 1;

    copy_text(params, tool->params, sizeof(params));

    char names[ARGS_COUNT][KEY_MAX];
    int count = 0;

    for (char *p = params; *p && count < ARGS_COUNT;)
    {
        int n = 0;

        while (p[n] && p[n] != ',')
        {
            n++;
        }

        for (int i = 0; i < n && i < KEY_MAX - 1; i++)
        {
            names[count][i] = p[i];
            names[count][i + 1] = 0;
        }

        count++;
        p += p[n] ? n + 1 : n;
    }

    for (int i = 0; i < count; i++)
    {
        add(t, first ? "\"" : ", \"");
        add(t, names[i]);
        add(t, "\": {\"type\": \"string\"");

        if (tool->run == tool_run_app && strcmp(names[i], "app") == 0)
        {
            add(t, ", \"enum\": [");

            for (int a = 0; a < app_count; a++)
            {
                add(t, a ? ", \"" : "\"");
                add(t, apps[a].name);
                add(t, "\"");
            }

            add(t, "]");
        }

        add(t, "}");
        first = 0;
    }

    add(t, "}, \"required\": [");
    first = 1;

    for (int i = 0; i < count; i++)
    {
        if (!optional_param(tool->name, names[i]))
        {
            add(t, first ? "\"" : ", \"");
            add(t, names[i]);
            add(t, "\"");
            first = 0;
        }
    }

    add(t, "]}}}\n");
}

static void build_system_prompt(void)
{
    text_t t = {system_prompt, 0, sizeof(system_prompt)};

    add(&t, SYSTEM_TEXT);
    add(&t, "\n\n# Tools\n\nYou may call one or more functions to assist with the user query.\n\n"
            "You are provided with function signatures within <tools></tools> XML tags:\n<tools>\n");

    for (int i = 0; i < TOOL_COUNT; i++)
    {
        add_tool_json(&t, &tools[i]);
    }

    add(&t, "</tools>\n\nFor each function call, return a json object with function name and arguments within "
            "<tool_call></tool_call> XML tags:\n<tool_call>\n{\"name\": <function-name>, \"arguments\": "
            "<args-json-object>}\n</tool_call>");
}

static const char *skip_space(const char *p)
{
    while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r')
    {
        p++;
    }

    return p;
}

static const char *parse_string(const char *p, char *out, int room)
{
    int n = 0;

    if (*p != '"')
    {
        return 0;
    }

    p++;

    while (*p && *p != '"')
    {
        char c = *p++;

        if (c == '\\' && *p)
        {
            c = *p++;
            c = c == 'n' ? '\n' : c == 't' ? '\t' : c;
        }

        if (n < room - 1)
        {
            out[n++] = c;
        }
    }

    out[n] = 0;
    return *p == '"' ? p + 1 : 0;
}

static const char *parse_value(const char *p, char *out, int room)
{
    p = skip_space(p);

    if (*p == '"')
    {
        return parse_string(p, out, room);
    }

    int n = 0;

    while (*p && *p != ',' && *p != '}' && *p != '\n')
    {
        if (n < room - 1)
        {
            out[n++] = *p;
        }

        p++;
    }

    while (n > 0 && out[n - 1] == ' ')
    {
        n--;
    }

    out[n] = 0;
    return p;
}

static int parse_call(const char *json, call_t *call)
{
    char key[KEY_MAX];
    const char *p = skip_space(json);

    memset(call, 0, sizeof(*call));

    if (*p++ != '{')
    {
        return -1;
    }

    while (1)
    {
        p = skip_space(p);

        if (*p == '}')
        {
            break;
        }

        if (!(p = parse_string(p, key, sizeof(key))))
        {
            return -1;
        }

        p = skip_space(p);

        if (*p++ != ':')
        {
            return -1;
        }

        p = skip_space(p);

        if (strcmp(key, "name") == 0)
        {
            if (!(p = parse_string(p, call->name, sizeof(call->name))))
            {
                return -1;
            }
        }
        else if (strcmp(key, "arguments") == 0 && *p == '{')
        {
            p++;

            while (1)
            {
                char akey[KEY_MAX];
                static char value[VALUE_MAX];

                p = skip_space(p);

                if (*p == '}')
                {
                    p++;
                    break;
                }

                if (!(p = parse_string(p, akey, sizeof(akey))))
                {
                    return -1;
                }

                p = skip_space(p);

                if (*p++ != ':' || !(p = parse_value(p, value, sizeof(value))))
                {
                    return -1;
                }

                set_arg(call, akey, value);
                p = skip_space(p);

                if (*p == ',')
                {
                    p++;
                }
            }
        }
        else if (!(p = parse_value(p, key, sizeof(key))))
        {
            return -1;
        }

        p = skip_space(p);

        if (*p == ',')
        {
            p++;
        }
    }

    return call->name[0] ? 0 : -1;
}

static void describe_call(const call_t *call)
{
    print(call->name);
    print("(");

    for (int i = 0; i < call->count; i++)
    {
        int length = (int)strlen(call->args[i].value);

        print(i ? ", " : "");
        print(call->args[i].key);
        print("=");
        write(FD_STDOUT, call->args[i].value, (unsigned long)(length > 60 ? 60 : length));
        print(length > 60 ? "..." : "");
    }

    print(")");
}

static int ask_user(void)
{
    char c;
    char answer = 0;

    print(" Allow? (y/n) ");

    while (read(FD_STDIN, &c, 1) == 1)
    {
        if (c == '\r' || c == '\n')
        {
            break;
        }

        if (answer == 0)
        {
            answer = lower(c);
        }

        write(FD_STDOUT, &c, 1);
    }

    print("\n");
    return answer == 'y';
}

static void execute(const call_t *call, text_t *out)
{
    const tool_t *tool = find_tool(call->name);

    if (!tool)
    {
        refuse(out, "unknown tool");
        print("[agent] refused unknown tool ");
        print(call->name);
        print("\n");
        return;
    }

    char params[64];
    int missing = 0;

    copy_text(params, tool->params, sizeof(params));

    for (char *p = params; *p;)
    {
        char *end = p;

        while (*end && *end != ',')
        {
            end++;
        }

        char saved = *end;

        *end = 0;

        if (!optional_param(tool->name, p) && !arg(call, p))
        {
            add(out, "error: missing argument ");
            add(out, p);
            missing = 1;
        }

        p = saved ? end + 1 : end;
    }

    char writes[64];

    copy_text(writes, tool->writes, sizeof(writes));

    for (char *p = writes; *p && !missing;)
    {
        char *end = p;

        while (*end && *end != ',')
        {
            end++;
        }

        char saved = *end;

        *end = 0;

        if (!writable_path(arg(call, p)))
        {
            add(out, "error: the agent may only change files inside /home and /tmp");
            missing = 1;
        }

        p = saved ? end + 1 : end;
    }

    if (!missing && tool->run == tool_run_app && !find_app(arg(call, "app")))
    {
        add(out, "error: no such app (apps are described in /etc/apps)");
        missing = 1;
    }

    if (missing)
    {
        print("[agent] ");
        describe_call(call);
        print(": ");
        print(out->data);
        print("\n");
        return;
    }

    const app_t *app = tool->run == tool_run_app ? find_app(arg(call, "app")) : 0;
    risk_t risk = app ? app->risk : tool->risk;

    print("[agent] ");
    describe_call(call);

    if (risk == RISK_CHANGE && !ask_user())
    {
        add(out, "the user did not allow this");
        print("[agent] skipped\n");
        return;
    }

    if (risk != RISK_CHANGE)
    {
        print("\n");
    }

    int result = tool->run(call, out);

    if (risk == RISK_CHANGE && result == 0)
    {
        char action[GRAPH_NAME_MAX];
        text_t a = {action, 0, sizeof(action)};

        add(&a, call->name);

        for (int i = 0; i < call->count; i++)
        {
            add(&a, " ");
            add(&a, call->args[i].value);
        }

        graph_record(GRAPH_KIND_ACTOR, "agent", GRAPH_REL_ACTION, GRAPH_KIND_ACTION, action, 100);
    }

    if (tool->run != tool_run_app)
    {
        print(out->data);
        print("\n");
    }
}

static int word_after(const char *request, const char *word, char *out, int room)
{
    for (int i = 0; request[i]; i++)
    {
        int j = 0;

        while (word[j] && lower(request[i + j]) == word[j])
        {
            j++;
        }

        if (word[j] == 0)
        {
            const char *p = request + i + j;
            int n = 0;

            while (*p == ' ')
            {
                p++;
            }

            while (p[n] && p[n] != ' ' && n < room - 1)
            {
                out[n] = p[n];
                n++;
            }

            out[n] = 0;
            return n > 0;
        }
    }

    return 0;
}

static int route(const char *request, call_t *calls)
{
    char word[64];
    int count = 0;

    memset(calls, 0, sizeof(call_t) * CALLS_MAX);

    if ((contains(request, "organize") || contains(request, "sort") || contains(request, "clean") ||
         contains(request, "tidy")) &&
        contains(request, "download"))
    {
        copy_text(calls[count].name, "run_app", sizeof(calls[count].name));
        set_arg(&calls[count], "app", "organize");
        set_arg(&calls[count], "args", contains(request, "undo") ? "/home/Downloads --undo" : "/home/Downloads --apply");
        count++;
    }

    if (word_after(request, "stop ", word, sizeof(word)) || word_after(request, "kill ", word, sizeof(word)))
    {
        copy_text(calls[count].name, "stop_program", sizeof(calls[count].name));
        set_arg(&calls[count], "name", word);
        count++;
    }

    if (count < CALLS_MAX && (contains(request, "slow") || contains(request, "wrong") ||
                              contains(request, "health") || contains(request, "status") ||
                              contains(request, "problem")))
    {
        copy_text(calls[count].name, "system_status", sizeof(calls[count].name));
        count++;
    }

    if (count == 0 && (word_after(request, "list ", word, sizeof(word)) || word_after(request, "show ", word, sizeof(word))) &&
        word[0] == '/')
    {
        copy_text(calls[count].name, "list_folder", sizeof(calls[count].name));
        set_arg(&calls[count], "path", word);
        count++;
    }

    if (count == 0 && word_after(request, "find ", word, sizeof(word)))
    {
        copy_text(calls[count].name, "find_files", sizeof(calls[count].name));
        set_arg(&calls[count], "name", word);
        count++;
    }

    return count;
}

static int allow_choice(int token, const unsigned char *text, unsigned int length, void *context)
{
    guide_t *g = (guide_t *)context;
    char candidate[48];
    int n = 0;

    (void)token;

    if (length == 0)
    {
        return 0;
    }

    for (int i = 0; g->partial[i] && n < 47; i++)
    {
        candidate[n++] = g->partial[i];
    }

    for (unsigned int i = 0; i < length && n < 47; i++)
    {
        candidate[n++] = (char)text[i];
    }

    candidate[n] = 0;

    for (int c = 0; c < g->choice_count; c++)
    {
        char full[40];
        text_t f = {full, 0, sizeof(full)};

        add(&f, g->choices[c]);
        add(&f, "\"");

        int m = 0;

        while (m < n && full[m] && candidate[m] == full[m])
        {
            m++;
        }

        if (m == n || full[m] == 0)
        {
            return 1;
        }
    }

    return 0;
}

static int ends_with(const char *text, int length, const char *suffix)
{
    int n = (int)strlen(suffix);

    return length >= n && memcmp_bytes(text + length - n, suffix, (unsigned long)n) == 0;
}

static void update_guide(int in_call, int length)
{
    if (guide.active)
    {
        if (contains(guide.partial, "\""))
        {
            guide.active = 0;
        }

        return;
    }

    if (!in_call)
    {
        return;
    }

    guide.choice_count = 0;

    if (ends_with(generated, length, "\"name\": \"") || ends_with(generated, length, "\"name\":\""))
    {
        for (int i = 0; i < TOOL_COUNT; i++)
        {
            guide.choices[guide.choice_count++] = tools[i].name;
        }
    }
    else if (ends_with(generated, length, "\"app\": \"") || ends_with(generated, length, "\"app\":\""))
    {
        for (int i = 0; i < app_count; i++)
        {
            guide.choices[guide.choice_count++] = apps[i].name;
        }
    }

    if (guide.choice_count > 0)
    {
        guide.active = 1;
        guide.partial[0] = 0;
    }
}

static int generate_step(void)
{
    int length = 0;
    int in_call = 0;
    int shown = 0;

    generated[0] = 0;
    guide.active = 0;

    for (int produced = 0; produced < STEP_TOKENS; produced++)
    {
        int token = llm_best_token(guide.active ? allow_choice : 0, &guide);
        unsigned int size;

        if (token < 0 || llm_is_end(token))
        {
            break;
        }

        const unsigned char *text = llm_token_text(token, &size);

        for (unsigned int i = 0; i < size && length < GENERATED_MAX - 1; i++)
        {
            generated[length++] = (char)text[i];
        }

        generated[length] = 0;

        if (guide.active)
        {
            text_t g = {guide.partial, (int)strlen(guide.partial), sizeof(guide.partial)};

            add_bytes(&g, (const char *)text, (int)size);
        }

        if (ends_with(generated, length, "<tool_call>"))
        {
            in_call = 1;
        }
        else if (ends_with(generated, length, "</tool_call>"))
        {
            in_call = 0;
        }
        else if (!in_call)
        {
            if (!shown)
            {
                print("[agent] ");
                shown = 1;
            }

            write(FD_STDOUT, text, size);
        }

        update_guide(in_call, length);

        if (llm_accept(token) != 0)
        {
            return -1;
        }
    }

    if (shown)
    {
        print("\n");
    }

    return llm_feed_special(LLM_IM_END) == 0 && llm_feed_text("\n") == 0 ? length : -1;
}

static int collect_calls(call_t *calls)
{
    int count = 0;
    const char *p = generated;

    while (count < CALLS_MAX)
    {
        const char *start = 0;

        for (const char *q = p; *q; q++)
        {
            if (starts_with(q, "<tool_call>"))
            {
                start = q + 11;
                break;
            }
        }

        if (!start)
        {
            break;
        }

        if (parse_call(start, &calls[count]) == 0)
        {
            count++;
        }
        else
        {
            print("[agent] could not read a tool call from the model\n");
        }

        p = start;
    }

    return count;
}

static int run_calls(call_t *calls, int count, int feed)
{
    for (int i = 0; i < count; i++)
    {
        text_t out = {output, 0, sizeof(output)};

        output[0] = 0;
        execute(&calls[i], &out);

        if (feed)
        {
            char observation[OBSERVATION_MAX + 1];
            int length = out.length < OBSERVATION_MAX ? out.length : OBSERVATION_MAX;

            memcpy(observation, output, (unsigned long)length);
            observation[length] = 0;

            if ((i == 0 && (llm_feed_special(LLM_IM_START) != 0 || llm_feed_text("user\n") != 0)) ||
                llm_feed_text(i ? "\n<tool_response>\n" : "<tool_response>\n") != 0 ||
                llm_feed_text(observation) != 0 || llm_feed_text("\n</tool_response>") != 0)
            {
                return -1;
            }
        }
    }

    if (feed && (llm_feed_special(LLM_IM_END) != 0 || llm_feed_text("\n") != 0 ||
                 llm_feed_special(LLM_IM_START) != 0 || llm_feed_text("assistant\n") != 0))
    {
        return -1;
    }

    return 0;
}

static int run_model(const char *request)
{
    static call_t calls[CALLS_MAX];
    unsigned long started = uptime();

    if (!llm_ready)
    {
        if (llm_load() != 0)
        {
            print("[agent] no language model (");
            print(llm_model_path());
            print("); make reset-disk DISK_MB=1024 puts Qwen on the disk\n");
            return 1;
        }

        llm_ready = 1;
    }

    build_system_prompt();
    llm_prompt_start(&prompt, system_prompt);

    int prefix = prompt.count;

    llm_prompt_text(&prompt, request);
    llm_prompt_end(&prompt, 0);
    print("[agent] thinking with ");
    print(llm_model_path());
    print(" (");
    print_uint((unsigned long)prompt.count);
    print(" prompt tokens)...\n");
    llm_read_prompt(&prompt, prefix, PREFIX_PATH);

    for (int step = 0; step < STEPS_MAX; step++)
    {
        if (generate_step() < 0)
        {
            print("[agent] out of context space, stopping\n");
            return 1;
        }

        int count = collect_calls(calls);

        if (count == 0)
        {
            print("[agent] done in ");
            print_uint((uptime() - started) / 100);
            print(" s\n");
            return 0;
        }

        if (run_calls(calls, count, 1) != 0)
        {
            print("[agent] out of context space, stopping\n");
            return 1;
        }
    }

    print("[agent] stopped after ");
    print_uint(STEPS_MAX);
    print(" steps\n");
    return 1;
}

static void list_tools(void)
{
    print("Tools:\n");

    for (int i = 0; i < TOOL_COUNT; i++)
    {
        print("  ");
        print(tools[i].name);
        print("(");
        print(tools[i].params);
        print(")");
        print(tools[i].risk == RISK_CHANGE ? "  asks first: " : "  ");
        print(tools[i].description);
        print("\n");
    }

    print("Apps (from " APPS_DIR "):\n");

    for (int i = 0; i < app_count; i++)
    {
        print("  ");
        print(apps[i].name);
        print(apps[i].risk == RISK_CHANGE ? "  asks first: " : "  ");
        print(apps[i].description);
        print("\n");
    }
}

int main(void)
{
    char request[ARGS_MAX];
    static call_t calls[CALLS_MAX];

    getargs(request, sizeof(request));
    load_apps();

    if (request[0] == 0)
    {
        print("usage: agent TASK | agent --llm TASK | agent --call JSON | agent --tools\n");
        return 1;
    }

    if (strcmp(request, "--tools") == 0)
    {
        list_tools();
        return 0;
    }

    if (starts_with(request, "--call "))
    {
        if (parse_call(request + 7, &calls[0]) != 0)
        {
            print("[agent] bad tool call, expected {\"name\": ..., \"arguments\": {...}}\n");
            return 1;
        }

        return run_calls(calls, 1, 0);
    }

    if (starts_with(request, "--llm "))
    {
        return run_model(request + 6);
    }

    int count = route(request, calls);

    if (count > 0)
    {
        print("[agent] plan (rules, no model needed):");

        for (int i = 0; i < count; i++)
        {
            print(" ");
            print(calls[i].name);
        }

        print("\n");
        run_calls(calls, count, 0);
        print("[agent] done\n");
        return 0;
    }

    return run_model(request);
}
