#include "desktop.h"
#include "../assist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#define ENTRY_MAX 64
#define ENTRY_TEXT 600
#define RESULT_MAX 16
#define NAME_INDEX_MAX 400
#define ACTIVITY_MAX 32

enum
{
    ENTRY_YOU,
    ENTRY_KNOC,
    ENTRY_LOG,
    ENTRY_CARD,
};

enum
{
    CARD_PENDING,
    CARD_ALLOWED,
    CARD_DENIED,
};

typedef struct entry
{
    int kind;
    int status;
    char tool[32];
    char text[ENTRY_TEXT];
    char stamp[12];
} entry_t;

typedef struct activity
{
    char what[160];
    char from[160];
    char to[160];
    int undone;
} activity_t;

enum
{
    RESULT_APP,
    RESULT_SETTING,
    RESULT_FILE,
    RESULT_MEANING,
    RESULT_WEB,
    RESULT_ASK,
    RESULT_RUN,
};

typedef struct result
{
    int kind;
    char title[160];
    char detail[96];
    char target[200];
    const app_t *app;
    int score;
} result_t;

static int assist_on;
static int assist_tab;
static int composer_focus;
static char composer[400];
static entry_t entries[ENTRY_MAX];
static int entry_count;
static int assist_scroll;
static activity_t activity[ACTIVITY_MAX];
static int activity_count;
static char always_allowed[8][32];
static int always_count;
static int health_seen;
static volatile int worker_busy;
static volatile int ask_pending;
static char ask_text[400];
static char line_buffer[ENTRY_TEXT];
static int line_length;
static int model_state;
static char model_line[64];
static rect_t card_buttons[ENTRY_MAX][3];
static rect_t tab_rects[3];
static rect_t undo_rects[ACTIVITY_MAX];
static uint64_t last_pulse;

static int bar_on;
static char query[160];
static result_t results[RESULT_MAX];
static int result_count;
static int selected;
static int scope;
static char name_index[NAME_INDEX_MAX][160];
static int name_count;
static volatile int find_pending;
static volatile int find_done;
static char find_query[160];
static char find_output[4096];
static char meaning_for[160];
static uint64_t last_typed;
static image_t preview_image;
static char preview_image_path[200];
static int preview_image_ok;
static rect_t bar_box;
static int chat_pty;
static int chat_pid;
static int chat_ready;
static int chat_skip_echo;
static int chat_live;
static char chat_waiting[400];
static char chat_line[ENTRY_TEXT];
static int chat_length;
static int card_chat[ENTRY_MAX];

static const char *const scopes[] = {"All", "Files", "Apps", "Ask"};

static void stamp_now(char *out)
{
    time_t now = time(0);

    if (now > 0)
    {
        struct tm *t = gmtime(&now);

        snprintf(out, 12, "%02d:%02d", t->tm_hour, t->tm_min);
    }
    else
    {
        snprintf(out, 12, "%lus", (unsigned long)(uptime() / 100));
    }
}

static entry_t *entry_add(int kind, const char *text)
{
    if (entry_count == ENTRY_MAX)
    {
        memmove(entries, entries + 1, sizeof(entry_t) * (ENTRY_MAX - 1));
        entry_count--;
    }

    entry_t *e = &entries[entry_count++];

    memset(e, 0, sizeof(*e));
    e->kind = kind;
    snprintf(e->text, sizeof(e->text), "%s", text);
    stamp_now(e->stamp);
    assist_scroll = 0;
    damage(screen_w - assist_width(), 0, assist_width(), screen_h);
    return e;
}

static void output_line(const char *line)
{
    if (!line[0])
    {
        return;
    }

    if (strncmp(line, "[knoc] ", 7) == 0)
    {
        entry_add(ENTRY_LOG, line + 7);
        return;
    }

    if (entry_count && entries[entry_count - 1].kind == ENTRY_KNOC)
    {
        entry_t *e = &entries[entry_count - 1];
        int length = (int)strlen(e->text);

        snprintf(e->text + length, sizeof(e->text) - (size_t)length, "%s%s", length ? " " : "", line);
        damage(screen_w - assist_width(), 0, assist_width(), screen_h);
    }
    else
    {
        entry_add(ENTRY_KNOC, line);
    }
}

static void output_hook(const char *text, int length)
{
    for (int i = 0; i < length; i++)
    {
        if (text[i] == '\n' || line_length >= ENTRY_TEXT - 1)
        {
            line_buffer[line_length] = 0;
            output_line(line_buffer);
            line_length = 0;
        }
        else
        {
            line_buffer[line_length++] = text[i];
        }
    }
}

static void flush_output(void)
{
    if (line_length)
    {
        line_buffer[line_length] = 0;
        output_line(line_buffer);
        line_length = 0;
    }
}

static int approve_hook(const char *tool, const char *description)
{
    flush_output();

    for (int i = 0; i < always_count; i++)
    {
        if (strcmp(always_allowed[i], tool) == 0)
        {
            entry_t *e = entry_add(ENTRY_CARD, description);

            snprintf(e->tool, sizeof(e->tool), "%s", tool);
            e->status = CARD_ALLOWED;
            return 1;
        }
    }

    entry_t *e = entry_add(ENTRY_CARD, description);
    int index = (int)(e - entries);

    snprintf(e->tool, sizeof(e->tool), "%s", tool);
    e->status = CARD_PENDING;

    char text[240];

    snprintf(text, sizeof(text), "Knoc asks: %s", tool);
    desktop_log(text);

    while (entries[index].status == CARD_PENDING)
    {
        sleep(5);
    }

    if (entries[index].status == CARD_ALLOWED && strcmp(tool, "move") == 0 && activity_count < ACTIVITY_MAX)
    {
        activity_t *a = &activity[activity_count++];
        char copy[ENTRY_TEXT];

        snprintf(copy, sizeof(copy), "%s", entries[index].text);

        char *from = strstr(copy, "from: ");
        char *to = strstr(copy, "to: ");

        memset(a, 0, sizeof(*a));

        if (from && to)
        {
            char *end = strchr(from, '\n');

            if (end)
            {
                *end = 0;
            }

            end = strchr(to, '\n');

            if (end)
            {
                *end = 0;
            }

            snprintf(a->from, sizeof(a->from), "%s", from + 6);
            snprintf(a->to, sizeof(a->to), "%s", to + 4);
            snprintf(a->what, sizeof(a->what), "Moved %s", a->from);
        }
    }

    return entries[index].status == CARD_ALLOWED;
}

static int file_exists(const char *path)
{
    file_stat_t st;

    return stat(path, &st) == 0;
}

static void *ask_worker(void *argument)
{
    (void)argument;

    while (1)
    {
        if (!ask_pending)
        {
            sleep(5);
            continue;
        }

        char question[400];

        snprintf(question, sizeof(question), "%s", ask_text);
        ask_pending = 0;
        worker_busy = 1;

        if (model_state == 0)
        {
            model_state = file_exists("/models/qwen.kllm") && assist_load_model() == 0 ? 1 : 2;
            snprintf(model_line, sizeof(model_line), "%s", model_state == 1 ? "qwen2.5 0.5B · on this machine" : "no model on this disk");
        }

        assist_call_t calls[ASSIST_CALLS_MAX];
        int routed = assist_route(question, calls);

        if (model_state == 1)
        {
            static int started;

            if (!started || assist_continue(question, 1) != 0)
            {
                assist_begin(question, 1);
                started = 1;
            }

            assist_reply();
            flush_output();
        }
        else if (routed > 0)
        {
            assist_run_calls(calls, routed);
            flush_output();
            entry_add(ENTRY_KNOC, "Done. Without a language model I run direct commands like this one right away.");
        }
        else
        {
            entry_add(ENTRY_KNOC, "There is no language model on this disk, so I can only run direct commands: "
                                  "\"find the invoice\", \"move /home/a.txt to /home/Documents\", \"stop spin\", "
                                  "\"organize my downloads\". Put qwen.kllm in /models to talk freely.");
        }

        worker_busy = 0;
        damage_all();
    }

    return 0;
}

static void *find_worker(void *argument)
{
    (void)argument;

    while (1)
    {
        if (!find_pending)
        {
            sleep(5);
            continue;
        }

        char q[160];

        snprintf(q, sizeof(q), "%s", find_query);
        find_pending = 0;

        int pid = spawn_capture("find", q, 2);

        if (pid >= 0)
        {
            wait(pid);

            long n = captured(find_output, sizeof(find_output) - 1);

            find_output[n > 0 ? n : 0] = 0;
        }
        else
        {
            find_output[0] = 0;
        }

        snprintf(meaning_for, sizeof(meaning_for), "%s", q);
        find_done = 1;
    }

    return 0;
}

void knoc_init(void)
{
    pthread_t a;
    pthread_t b;

    assist_init("[knoc] ");
    assist_set_hooks(output_hook, approve_hook);
    snprintf(model_line, sizeof(model_line), "%s",
             file_exists("/models/qwen.kllm") ? "qwen2.5 0.5B · on this machine" : "no model on this disk");
    pthread_create(&a, 0, ask_worker, 0);
    pthread_create(&b, 0, find_worker, 0);

    crash_info_t info;

    while (crashinfo((unsigned long)health_seen, &info) == 0)
    {
        health_seen++;
    }
}

int assist_open(void)
{
    return assist_on;
}

int assist_width(void)
{
    return assist_on ? SI(388) : 0;
}

void assist_toggle(void)
{
    assist_on = !assist_on;
    composer_focus = assist_on;
    damage_all();
    desktop_log(assist_on ? "assist open" : "assist closed");

    for (int i = 0; i < WINDOW_MAX; i++)
    {
        (void)i;
    }
}

static void chat_send(void)
{
    if (!chat_ready || !chat_waiting[0])
    {
        return;
    }

    pty_write(chat_pty, chat_waiting, strlen(chat_waiting));
    pty_write(chat_pty, "\r", 1);
    chat_waiting[0] = 0;
    chat_ready = 0;
    chat_skip_echo = 1;
    chat_live = -1;
}

static int chat_start(void)
{
    if (chat_pty > 0)
    {
        return 0;
    }

    chat_pty = pty_open();

    if (chat_pty < 0)
    {
        chat_pty = 0;
        return -1;
    }

    chat_pid = pty_spawn(chat_pty, "chat", "");

    if (chat_pid < 0)
    {
        chat_pty = 0;
        return -1;
    }

    chat_ready = 0;
    chat_length = 0;
    snprintf(model_line, sizeof(model_line), "qwen2.5 0.5B · on the AI cores");
    return 0;
}

void assist_ask(const char *question)
{
    if (!assist_on)
    {
        assist_toggle();
    }

    entry_add(ENTRY_YOU, question);

    char text[460];

    snprintf(text, sizeof(text), "asked Knoc: %s", question);
    desktop_log(text);

    if (file_exists("/models/qwen.kllm") && chat_start() == 0)
    {
        snprintf(chat_waiting, sizeof(chat_waiting), "%s", question);
        worker_busy = 1;
        chat_send();
        return;
    }

    snprintf(ask_text, sizeof(ask_text), "%s", question);
    ask_pending = 1;
}

static int tool_line(const char *text, char *tool, int room)
{
    int n = 0;

    while (text[n] && ((text[n] >= 'a' && text[n] <= 'z') || text[n] == '_') && n < room - 1)
    {
        tool[n] = text[n];
        n++;
    }

    tool[n] = 0;
    return n > 2 && text[n] == '(';
}

static void tool_description(const char *call, char *out, int room)
{
    const char *open = strchr(call, '(');
    const char *close = strrchr(call, ')');
    int n = 0;

    out[0] = 0;

    if (!open || !close || close < open)
    {
        return;
    }

    for (const char *p = open + 1; p < close && n < room - 3; p++)
    {
        if (*p == '=')
        {
            out[n++] = ':';
            out[n++] = ' ';
        }
        else if (*p == ',' && p[1] == ' ')
        {
            out[n++] = '\n';
            p++;
        }
        else
        {
            out[n++] = *p;
        }
    }

    out[n] = 0;
}

static void chat_complete_line(char *line)
{
    char tool[32];

    if (chat_skip_echo)
    {
        chat_skip_echo = 0;
        return;
    }

    if (strncmp(line, "knocos: ", 8) == 0)
    {
        const char *rest = line + 8;

        if (tool_line(rest, tool, sizeof(tool)) || strncmp(rest, "using ", 6) == 0 ||
            strncmp(rest, "reading ", 8) == 0 || strcmp(rest, "skipped") == 0)
        {
            if (chat_live >= 0 && chat_live < entry_count && entries[chat_live].kind == ENTRY_KNOC &&
                strcmp(entries[chat_live].text, rest) == 0)
            {
                entries[chat_live].kind = ENTRY_LOG;
            }
            else
            {
                entry_add(ENTRY_LOG, rest);
            }

            chat_live = -1;
            return;
        }

        if (chat_live >= 0 && chat_live < entry_count)
        {
            snprintf(entries[chat_live].text, sizeof(entries[chat_live].text), "%s", rest);
        }
        else if (rest[0])
        {
            entry_add(ENTRY_KNOC, rest);
        }

        chat_live = -1;
        return;
    }

    if (line[0] == '(' || strncmp(line, "chat: ", 6) == 0)
    {
        if (strncmp(line, "chat: ", 6) == 0)
        {
            entry_add(ENTRY_LOG, line + 6);
        }

        chat_live = -1;
        return;
    }

    if (line[0] && entry_count && entries[entry_count - 1].kind == ENTRY_KNOC)
    {
        entry_t *e = &entries[entry_count - 1];
        int length = (int)strlen(e->text);

        snprintf(e->text + length, sizeof(e->text) - (size_t)length, " %s", line);
    }
}

static void chat_partial(void)
{
    char tool[32];

    chat_line[chat_length] = 0;

    if (strcmp(chat_line, "you: ") == 0)
    {
        chat_length = 0;
        chat_ready = 1;
        worker_busy = 0;
        chat_live = -1;
        chat_send();
        damage_all();
        return;
    }

    char *allow = strstr(chat_line, " Allow? (y/n) ");

    if (allow && strncmp(chat_line, "knocos: ", 8) == 0)
    {
        *allow = 0;

        char description[400];

        tool_line(chat_line + 8, tool, sizeof(tool));
        tool_description(chat_line + 8, description, sizeof(description));

        entry_t *e = 0;

        for (int i = 0; i < always_count; i++)
        {
            if (strcmp(always_allowed[i], tool) == 0)
            {
                e = entry_add(ENTRY_CARD, description);
                snprintf(e->tool, sizeof(e->tool), "%s", tool);
                e->status = CARD_ALLOWED;
                pty_write(chat_pty, "y\r", 2);
            }
        }

        if (!e)
        {
            e = entry_add(ENTRY_CARD, description);
            snprintf(e->tool, sizeof(e->tool), "%s", tool);
            e->status = CARD_PENDING;
            card_chat[e - entries] = 1;
            desktop_log("Knoc asks through chat");
        }

        chat_skip_echo = 1;
        chat_length = 0;
        return;
    }

    if (strncmp(chat_line, "knocos: ", 8) == 0 && !tool_line(chat_line + 8, tool, sizeof(tool)) && chat_length > 8)
    {
        if (chat_live < 0 || chat_live >= entry_count || entries[chat_live].kind != ENTRY_KNOC)
        {
            chat_live = (int)(entry_add(ENTRY_KNOC, chat_line + 8) - entries);
        }
        else
        {
            snprintf(entries[chat_live].text, sizeof(entries[chat_live].text), "%s", chat_line + 8);
        }

        damage(screen_w - assist_width(), 0, assist_width(), screen_h);
    }
}

static void chat_tick(void)
{
    char chunk[1024];

    if (!chat_pty)
    {
        return;
    }

    long n = pty_read(chat_pty, chunk, sizeof(chunk));

    if (n <= 0)
    {
        return;
    }

    for (long i = 0; i < n; i++)
    {
        char c = chunk[i];

        if (c == '\r')
        {
            continue;
        }

        if (c == '\n')
        {
            chat_line[chat_length] = 0;
            chat_complete_line(chat_line);
            chat_length = 0;
            continue;
        }

        if (c == '\b')
        {
            if (chat_length)
            {
                chat_length--;
            }

            continue;
        }

        if (chat_length < ENTRY_TEXT - 1)
        {
            chat_line[chat_length++] = c;
        }
    }

    chat_partial();
}

static int wrap_lines(font_t *font, float size, const char *text, int width, char lines[][160], int max)
{
    int count = 0;
    const char *p = text;

    while (*p && count < max)
    {
        int best = 0;
        int i = 0;
        char trial[160];

        while (p[i] && p[i] != '\n' && i < 158)
        {
            memcpy(trial, p, (size_t)i + 1);
            trial[i + 1] = 0;

            if (text_width(font, size, trial) > width)
            {
                break;
            }

            if (p[i] == ' ')
            {
                best = i;
            }

            i++;
        }

        int take = (!p[i] || p[i] == '\n' || i >= 158) ? i : best ? best : i;

        memcpy(lines[count], p, (size_t)take);
        lines[count][take] = 0;
        count++;
        p += take;

        while (*p == ' ' || *p == '\n')
        {
            p++;
        }
    }

    return count;
}

static const char *tool_title(const char *tool)
{
    if (strcmp(tool, "move") == 0)
    {
        return "Move a file";
    }

    if (strcmp(tool, "write_file") == 0)
    {
        return "Write a file";
    }

    if (strcmp(tool, "stop_program") == 0)
    {
        return "Stop a program";
    }

    if (strcmp(tool, "run_app") == 0)
    {
        return "Run an app";
    }

    if (strcmp(tool, "copy") == 0)
    {
        return "Copy a file";
    }

    if (strcmp(tool, "make_folder") == 0)
    {
        return "Make a folder";
    }

    return tool;
}

static int entry_height(entry_t *e, int width)
{
    char lines[16][160];

    if (e->kind == ENTRY_LOG)
    {
        return 18;
    }

    if (e->kind == ENTRY_CARD)
    {
        int n = wrap_lines(font_mono, S(10.8f), e->text, width - 24, lines, 8);

        return 34 + n * 18 + (e->status == CARD_PENDING ? 44 : 30) + 10;
    }

    int n = wrap_lines(font_ui, S(13.5f), e->text, width - (e->kind == ENTRY_KNOC ? 12 : 0), lines, 16);

    return 20 + n * SI(20) + 12;
}

static void draw_entry(canvas_t *c, entry_t *e, int index, int x, int y, int width)
{
    char lines[16][160];

    for (int k = 0; k < 3; k++)
    {
        card_buttons[index][k] = (rect_t){0, 0, 0, 0};
    }

    if (e->kind == ENTRY_LOG)
    {
        char text[160];

        draw_text(c, font_mono, S(10.8f), x + 12, y + 12, e->stamp, T.i3);
        text_fit(text, sizeof(text), font_mono, S(10.8f), e->text, width - 70);
        draw_text(c, font_mono, S(10.8f), x + 58, y + 12, text, T.i2);
        return;
    }

    if (e->kind == ENTRY_CARD)
    {
        int n = wrap_lines(font_mono, S(10.8f), e->text, width - 24, lines, 8);
        int h = entry_height(e, width) - 10;
        uint32_t edge = e->status == CARD_PENDING ? T.need : T.ln2;

        draw_round(c, x, y, width, h, 6, T.w, 255);
        draw_blend(c, x + 1, y + 1, width - 2, 32, T.need, e->status == CARD_PENDING ? 28 : 0);
        draw_frame(c, x, y, width, h, 6, edge, 255);
        draw_text(c, font_bold, S(12.5f), x + 10, y + 21, tool_title(e->tool), T.i);

        const char *state = e->status == CARD_PENDING ? "NEEDS YOU" : e->status == CARD_ALLOWED ? "ALLOWED" : "DENIED";
        int sw = text_width(font_label, S(9.5f), state);

        draw_text(c, font_label, S(9.5f), x + width - sw - 10, y + 21, state,
                  e->status == CARD_PENDING ? T.need : e->status == CARD_ALLOWED ? T.ok : T.i3);
        draw_fill(c, x + 1, y + 32, width - 2, 1, T.ln);

        for (int k = 0; k < n; k++)
        {
            draw_text(c, font_mono, S(10.8f), x + 12, y + 50 + k * 18, lines[k], T.i2);
        }

        if (e->status == CARD_PENDING)
        {
            int by = y + 50 + n * 18;
            int bx = x + 10;
            int w1 = ui_button(c, bx, by, "Allow", 1);

            card_buttons[index][0] = (rect_t){bx, by, w1, SI(28)};
            bx += w1 + 6;

            int w2 = ui_button(c, bx, by, "Deny", 0);

            card_buttons[index][1] = (rect_t){bx, by, w2, SI(28)};

            char always[48];

            snprintf(always, sizeof(always), "Always allow");

            int aw = text_width(font_ui, S(12), always);

            draw_text(c, font_ui, S(12), x + width - aw - 12, by + SI(18), always, T.i3);
            card_buttons[index][2] = (rect_t){x + width - aw - 16, by, aw + 8, SI(28)};
        }

        return;
    }

    const char *who = e->kind == ENTRY_YOU ? "YOU" : "KNOC";

    draw_text(c, font_label, S(9.5f), x + (e->kind == ENTRY_KNOC ? 12 : 0), y + 12, who, T.i3);
    draw_text(c, font_label, S(9.5f), x + width - text_width(font_label, S(9.5f), e->stamp), y + 12, e->stamp, T.i3);

    int n = wrap_lines(font_ui, S(13.5f), e->text, width - (e->kind == ENTRY_KNOC ? 12 : 0), lines, 16);

    if (e->kind == ENTRY_KNOC)
    {
        draw_fill(c, x, y + 2, 1, 20 + n * SI(20), T.ln2);
    }

    for (int k = 0; k < n; k++)
    {
        draw_text(c, font_ui, S(13.5f), x + (e->kind == ENTRY_KNOC ? 12 : 0), y + 34 + k * SI(20), lines[k], T.i);
    }
}

static int health_new(void)
{
    crash_info_t info;
    int count = 0;

    while (crashinfo((unsigned long)(health_seen + count), &info) == 0)
    {
        count++;
    }

    return count;
}

void assist_draw(canvas_t *view, int ox, int oy)
{
    if (!assist_on)
    {
        return;
    }

    int w = assist_width();
    int x0 = screen_w - w;
    int x = x0 - ox;
    int y = -oy;

    if (x0 - ox >= view->width || x0 + w - ox <= 0)
    {
        return;
    }

    draw_fill(view, x, y, w, screen_h, T.w);
    draw_fill(view, x, y, 1, screen_h, T.ln);
    presence_draw(view, x + 16, y + 20, worker_busy);
    draw_text(view, font_bold, S(14), x + 38, y + 32, "Knoc", T.i);

    char model[64];

    snprintf(model, sizeof(model), "%s", model_line);
    draw_text(view, font_mono, S(10), x + w - 14 - text_width(font_mono, S(10), model), y + 24, model, T.i3);
    draw_text(view, font_mono, S(10), x + w - 14 - text_width(font_mono, S(10), "AI cores 5-7"), y + 38, "AI cores 5-7", T.i3);
    draw_fill(view, x, y + 52, w, 1, T.ln);

    static const char *const tabs[] = {"Session", "Activity", "Health"};
    int tx = x + 14;
    int fresh = health_new();

    for (int k = 0; k < 3; k++)
    {
        int tw = text_width(font_ui, S(13), tabs[k]);
        int on = k == assist_tab;

        draw_text(view, on ? font_bold : font_ui, S(13), tx, y + 76, tabs[k], on ? T.i : T.i3);

        if (on)
        {
            draw_fill(view, tx, y + 88, tw, 2, T.i);
        }

        tab_rects[k] = (rect_t){tx + ox, y + 56 + oy, tw + 12, 34};
        tx += tw;

        if (k == 2 && fresh)
        {
            char count[8];

            snprintf(count, sizeof(count), "%d", fresh);
            draw_text(view, font_mono, S(10), tx + 4, y + 76, count, T.need);
            tx += 14;
        }

        tx += 18;
    }

    draw_fill(view, x, y + 90, w, 1, T.ln);

    window_t *focused = window_focused();
    char sees[200];

    if (focused)
    {
        snprintf(sees, sizeof(sees), "%s · %s", focused->title, focused->crumb);
    }
    else
    {
        snprintf(sees, sizeof(sees), "the desktop");
    }

    char fitted[200];

    draw_label(view, x + 14, y + 112, "Sees", T.i3);
    text_fit(fitted, sizeof(fitted), font_ui, S(12), sees, w - 80);
    draw_text(view, font_ui, S(12), x + 60, y + 112, fitted, T.i2);
    draw_label(view, x + 14, y + 130, "Can", T.i3);
    draw_text(view, font_ui, S(12), x + 60, y + 130, "read files, search, change things with your OK", T.i2);
    draw_fill(view, x, y + 142, w, 1, T.ln);

    int top = y + 150;
    int bottom = y + screen_h - 86;
    int inner = w - 28;

    if (assist_tab == 0)
    {
        int total = 0;
        int start = entry_count;

        while (start > 0 && total + entry_height(&entries[start - 1], inner) < bottom - top)
        {
            start--;
            total += entry_height(&entries[start], inner);
        }

        int cy = top + 6;

        if (entry_count == 0)
        {
            char lines[8][160];
            int n = wrap_lines(font_ui, S(13), "Ask anything, or give a direct command like \"find the invoice\" or \"move /home/notes.txt to /home/Documents\". Anything that changes your files shows up here first, for you to allow.", inner, lines, 8);

            for (int k = 0; k < n; k++)
            {
                draw_text(view, font_ui, S(13), x + 14, cy + 20 + k * SI(20), lines[k], T.i3);
            }
        }

        for (int i = start; i < entry_count; i++)
        {
            draw_entry(view, &entries[i], i, x + 14, cy, inner);

            for (int k = 0; k < 3; k++)
            {
                card_buttons[i][k].x += ox;
                card_buttons[i][k].y += oy;
            }

            cy += entry_height(&entries[i], inner);
        }
    }
    else if (assist_tab == 1)
    {
        int cy = top + 24;

        if (activity_count == 0)
        {
            draw_text(view, font_ui, S(13), x + 14, cy, "Changes you allow appear here, each one with Undo.", T.i3);
        }

        for (int i = activity_count - 1; i >= 0 && cy < bottom; i--)
        {
            activity_t *a = &activity[i];
            char what[160];

            text_fit(what, sizeof(what), font_ui, S(13), a->what, inner - 70);
            draw_text(view, font_ui, S(13), x + 14, cy, what, a->undone ? T.i3 : T.i);
            text_fit(what, sizeof(what), font_mono, S(10.5f), a->to, inner - 70);
            draw_text(view, font_mono, S(10.5f), x + 14, cy + 17, what, T.i3);
            undo_rects[i] = (rect_t){0, 0, 0, 0};

            if (!a->undone)
            {
                int bw = ui_button(view, x + w - 80, cy - 16, "Undo", 0);

                undo_rects[i] = (rect_t){x + w - 80 + ox, cy - 16 + oy, bw, SI(28)};
            }
            else
            {
                draw_text(view, font_label, S(9.5f), x + w - 76, cy, "UNDONE", T.i3);
            }

            cy += 44;
        }
    }
    else
    {
        int cy = top + 24;
        crash_info_t info;
        int shown = 0;

        for (int i = 0; crashinfo((unsigned long)i, &info) == 0 && cy < bottom; i++)
        {
            char line[200];

            snprintf(line, sizeof(line), "%s crashed", info.process[0] ? info.process : info.driver);
            draw_round(view, x + 14, cy - 9, 6, 6, 1, T.bad, 255);
            draw_text(view, font_bold, S(13), x + 28, cy, line, T.i);
            text_fit(line, sizeof(line), font_ui, S(12), info.diagnosis[0] ? info.diagnosis : info.message, inner - 16);
            draw_text(view, font_ui, S(12), x + 28, cy + 18, line, T.i2);
            cy += 46;
            shown++;
        }

        if (!shown)
        {
            draw_text(view, font_ui, S(13), x + 14, cy, "No problems. The AI space watches every program and driver.", T.i3);
        }
    }

    int cx = x + 12;
    int cyb = y + screen_h - 78;
    int cw = w - 24;

    draw_round(view, cx, cyb, cw, 64, 6, T.w2, 255);
    draw_frame(view, cx, cyb, cw, 64, 6, composer_focus ? T.a : T.ln2, 255);

    char text[400];

    if (composer[0])
    {
        text_fit(text, sizeof(text), font_ui, S(13), composer, cw - 30);

        int tw = draw_text(view, font_ui, S(13), cx + 11, cyb + 24, text, T.i);

        if (composer_focus)
        {
            draw_fill(view, cx + 12 + tw, cyb + 11, 2, 17, T.a);
        }
    }
    else
    {
        draw_text(view, font_ui, S(13), cx + 11, cyb + 24, worker_busy ? "Knoc is working…" : "Ask Knoc, or give a command", T.i3);

        if (composer_focus)
        {
            draw_fill(view, cx + 11, cyb + 11, 2, 17, T.a);
        }
    }

    draw_text(view, font_label, S(9.5f), cx + cw - 12 - text_width(font_label, S(9.5f), "ENTER TO SEND"), cyb + 50,
              "ENTER TO SEND", T.i3);
}

int assist_event(const input_event_t *e)
{
    if (!assist_on)
    {
        return 0;
    }

    int x0 = screen_w - assist_width();

    if (e->type == INPUT_BUTTON)
    {
        if (e->x < x0)
        {
            composer_focus = 0;
            damage(x0, screen_h - 90, assist_width(), 90);
            return 0;
        }

        if (e->value != 1)
        {
            return 1;
        }

        for (int k = 0; k < 3; k++)
        {
            if (point_in(e->x, e->y, tab_rects[k].x, tab_rects[k].y, tab_rects[k].w, tab_rects[k].h))
            {
                assist_tab = k;

                if (k == 2)
                {
                    health_seen += health_new();
                }

                damage(x0, 0, assist_width(), screen_h);
                return 1;
            }
        }

        for (int i = 0; i < entry_count; i++)
        {
            if (entries[i].kind != ENTRY_CARD || entries[i].status != CARD_PENDING)
            {
                continue;
            }

            for (int k = 0; k < 3; k++)
            {
                rect_t r = card_buttons[i][k];

                if (r.w && point_in(e->x, e->y, r.x, r.y, r.w, r.h))
                {
                    if (k == 2 && always_count < 8)
                    {
                        snprintf(always_allowed[always_count++], 32, "%s", entries[i].tool);
                    }

                    entries[i].status = k == 1 ? CARD_DENIED : CARD_ALLOWED;
                    desktop_log(k == 1 ? "change denied" : "change allowed");

                    if (card_chat[i])
                    {
                        card_chat[i] = 0;
                        pty_write(chat_pty, k == 1 ? "n\r" : "y\r", 2);
                    }
                    damage(x0, 0, assist_width(), screen_h);
                    return 1;
                }
            }
        }

        if (assist_tab == 1)
        {
            for (int i = 0; i < activity_count; i++)
            {
                rect_t r = undo_rects[i];

                if (r.w && !activity[i].undone && point_in(e->x, e->y, r.x, r.y, r.w, r.h))
                {
                    if (rename(activity[i].to, activity[i].from) == 0)
                    {
                        activity[i].undone = 1;
                        notify("Knoc", "Undone", activity[i].from);
                        desktop_log("change undone");
                    }
                    else
                    {
                        notify("Knoc", "Could not undo", activity[i].to);
                    }

                    damage(x0, 0, assist_width(), screen_h);
                    return 1;
                }
            }
        }

        composer_focus = e->y > screen_h - 90;
        damage(x0, 0, assist_width(), screen_h);
        return 1;
    }

    if (e->type == INPUT_WHEEL)
    {
        return e->x >= x0;
    }

    if (e->type != INPUT_KEY || !composer_focus || e->value == 0)
    {
        return 0;
    }

    int length = (int)strlen(composer);

    if (e->code == KEY_ENTER)
    {
        if (length && !worker_busy)
        {
            assist_tab = 0;
            assist_ask(composer);
            composer[0] = 0;
        }
    }
    else if (e->code == KEY_BACKSPACE)
    {
        if (length)
        {
            composer[length - 1] = 0;
        }
    }
    else if (e->code == KEY_ESC)
    {
        assist_toggle();
        return 1;
    }
    else if (e->text >= 32 && e->text < 127 && length < (int)sizeof(composer) - 1 && !(e->modifiers & INPUT_CTRL))
    {
        composer[length] = (char)e->text;
        composer[length + 1] = 0;
    }
    else
    {
        return 0;
    }

    damage(x0, screen_h - 90, assist_width(), 90);
    return 1;
}

void assist_tick(void)
{
    chat_tick();

    if (worker_busy && uptime() - last_pulse > 8)
    {
        last_pulse = uptime();
        damage(screen_w - assist_width(), 0, assist_width(), 60);
        damage(0, screen_h - STRIP_H - STRIP_GAP, screen_w, STRIP_H + STRIP_GAP);
    }

    if (find_done)
    {
        find_done = 0;

        if (bar_on)
        {
            extern void knoc_bar_rebuild(void);

            knoc_bar_rebuild();
        }
    }

    if (bar_on && query[0] && strcmp(query, meaning_for) != 0 && !find_pending && uptime() - last_typed > 45 &&
        strcmp(find_query, query) != 0)
    {
        snprintf(find_query, sizeof(find_query), "%s", query);
        find_pending = 1;
    }
}

static void index_names(const char *dir, int depth)
{
    dir_entry_t entry;

    for (unsigned long i = 0; name_count < NAME_INDEX_MAX && readdir(dir, i, &entry) == 0; i++)
    {
        char path[160];

        snprintf(path, sizeof(path), "%s/%s", strcmp(dir, "/") == 0 ? "" : dir, entry.name);

        if (entry.type == 2)
        {
            if (depth < 3)
            {
                index_names(path, depth + 1);
            }
        }
        else
        {
            snprintf(name_index[name_count++], 160, "%s", path);
        }
    }
}

int knoc_bar_open(void)
{
    return bar_on;
}

static const struct
{
    const char *name;
    const char *words;
    const char *detail;
    const app_t *app;
    const char *arg;
} launchers[] = {
    {"Terminal", "terminal shell knocsh bash command console", "knocsh, bash, Lua and BusyBox", &app_terminal, 0},
    {"Files", "files folders documents downloads explorer", "Your files, sorted by Knoc", &app_files, "/home"},
    {"Settings", "settings appearance theme accent wallpaper dark light paper graphite", "Theme, accent, wallpaper", &app_settings, 0},
    {"Text size", "text size bigger smaller font scale display zoom", "Settings › Display", &app_settings, "display"},
    {"About KnocOS", "about version system memory disk", "Settings › About", &app_settings, "about"},
    {"Monitor", "monitor processes cpu cores memory task manager", "8 cores and every program", &app_monitor, 0},
    {"Editor", "editor text notes write code", "Edit text and code", &app_editor, "/home/notes.txt"},
    {"Web", "web browser internet website google duckduckgo", "The text web browser, starting at DuckDuckGo", &app_terminal, "!web lite.duckduckgo.com"},
};

#define LAUNCHERS (int)(sizeof(launchers) / sizeof(launchers[0]))

static int match(const char *text, const char *q)
{
    for (const char *p = text; *p; p++)
    {
        int i = 0;

        while (q[i] && (p[i] | 0x20) == (q[i] | 0x20))
        {
            i++;
        }

        if (!q[i])
        {
            return 1;
        }
    }

    return 0;
}

void knoc_bar_rebuild(void)
{
    result_count = 0;

    if (query[0] && (scope == 0 || scope == 2))
    {
        for (int i = 0; i < LAUNCHERS && result_count < RESULT_MAX - 6; i++)
        {
            if (match(launchers[i].name, query) || match(launchers[i].words, query))
            {
                result_t *r = &results[result_count++];

                memset(r, 0, sizeof(*r));
                r->kind = launchers[i].arg && launchers[i].app == &app_settings ? RESULT_SETTING : RESULT_APP;
                r->app = launchers[i].app;
                snprintf(r->title, sizeof(r->title), "%s", launchers[i].name);
                snprintf(r->detail, sizeof(r->detail), "%s", launchers[i].detail);
                snprintf(r->target, sizeof(r->target), "%s", launchers[i].arg ? launchers[i].arg : "");
            }
        }
    }

    if (query[0] && (scope == 0 || scope == 1))
    {
        char copy[4096];

        snprintf(copy, sizeof(copy), "%s", strcmp(meaning_for, query) == 0 ? find_output : "");

        char *line = strtok(copy, "\n");

        int meaning_added = 0;

        while (line && meaning_added < 5 && result_count < RESULT_MAX - 6)
        {
            char *percent = strchr(line, '%');
            char *path = percent ? strchr(percent, '/') : 0;

            if (path && percent - line < 8)
            {
                char *end = path;

                while (*end && *end != ' ' && *end != '\t')
                {
                    end++;
                }

                *end = 0;

                if (file_exists(path))
                {
                    result_t *r = &results[result_count++];
                    const char *base = strrchr(path, '/');

                    memset(r, 0, sizeof(*r));
                    r->kind = RESULT_MEANING;
                    snprintf(r->title, sizeof(r->title), "%s", base ? base + 1 : path);
                    snprintf(r->target, sizeof(r->target), "%s", path);
                    snprintf(r->detail, sizeof(r->detail), "by meaning · %.80s", path);
                    meaning_added++;
                }
            }

            line = strtok(0, "\n");
        }
    }

    if (query[0] && (scope == 0 || scope == 1))
    {
        for (int i = 0; i < name_count && result_count < RESULT_MAX - 3; i++)
        {
            const char *base = strrchr(name_index[i], '/');

            if (match(base ? base + 1 : name_index[i], query))
            {
                int duplicate = 0;

                for (int k = 0; k < result_count; k++)
                {
                    duplicate |= strcmp(results[k].target, name_index[i]) == 0;
                }

                if (duplicate)
                {
                    continue;
                }

                result_t *r = &results[result_count++];

                memset(r, 0, sizeof(*r));
                r->kind = RESULT_FILE;
                snprintf(r->title, sizeof(r->title), "%s", base ? base + 1 : name_index[i]);
                snprintf(r->target, sizeof(r->target), "%s", name_index[i]);
                snprintf(r->detail, sizeof(r->detail), "by name · %.80s", name_index[i]);
            }
        }
    }

    if (query[0] && scope == 0)
    {
        int address = strchr(query, '.') && !strchr(query, ' ');
        result_t *r = &results[result_count++];

        memset(r, 0, sizeof(*r));
        r->kind = RESULT_WEB;

        if (address)
        {
            snprintf(r->title, sizeof(r->title), "Open %s", query);
            snprintf(r->detail, sizeof(r->detail), "The web browser, in a Terminal window");
            snprintf(r->target, sizeof(r->target), "!web %s", query);
        }
        else
        {
            snprintf(r->title, sizeof(r->title), "Search the web: %s", query);
            snprintf(r->detail, sizeof(r->detail), "DuckDuckGo, in the web browser");
            snprintf(r->target, sizeof(r->target), "!web -s %s", query);
        }
    }

    if (query[0] && (scope == 0 || scope == 3))
    {
        result_t *r = &results[result_count++];

        memset(r, 0, sizeof(*r));
        r->kind = RESULT_ASK;
        snprintf(r->title, sizeof(r->title), "Ask Knoc: %s", query);
        snprintf(r->detail, sizeof(r->detail), "The answer opens in Assist");
    }

    if (query[0] && scope == 0)
    {
        result_t *r = &results[result_count++];

        memset(r, 0, sizeof(*r));
        r->kind = RESULT_RUN;
        snprintf(r->title, sizeof(r->title), "Run in Terminal: %s", query);
        snprintf(r->detail, sizeof(r->detail), "knocsh");
    }

    if (selected >= result_count)
    {
        selected = result_count ? result_count - 1 : 0;
    }

    damage(bar_box.x, bar_box.y, bar_box.w, bar_box.h);
}

void knoc_bar_toggle(void)
{
    bar_on = !bar_on;

    if (bar_on)
    {
        query[0] = 0;
        selected = 0;
        scope = 0;
        result_count = 0;
        name_count = 0;
        index_names("/home", 0);
        desktop_log("knoc bar open");
    }

    damage_all();
}

static void activate(int index, int how)
{
    if (index < 0 || index >= result_count)
    {
        if (query[0] && how == 1)
        {
            char q[160];

            snprintf(q, sizeof(q), "%s", query);
            bar_on = 0;
            assist_ask(q);
        }

        return;
    }

    result_t r = results[index];
    char q[160];

    snprintf(q, sizeof(q), "%s", query);
    bar_on = 0;
    damage_all();

    if (how == 1)
    {
        assist_ask(q);
        return;
    }

    if (how == 2 || r.kind == RESULT_RUN)
    {
        char command[200];

        snprintf(command, sizeof(command), "!%s", q);
        window_open(&app_terminal, command);
        return;
    }

    char text[260];

    snprintf(text, sizeof(text), "knoc bar opened %s", r.target[0] ? r.target : r.title);
    desktop_log(text);

    switch (r.kind)
    {
    case RESULT_APP:
    case RESULT_SETTING:
        window_open(r.app, r.target[0] ? r.target : 0);
        break;
    case RESULT_FILE:
    case RESULT_MEANING:
        open_path(r.target);
        break;
    case RESULT_WEB:
        window_open(&app_terminal, r.target);
        break;
    case RESULT_ASK:
        assist_ask(q);
        break;
    default:
        break;
    }
}

int knoc_bar_event(const input_event_t *e)
{
    if (!bar_on)
    {
        return 0;
    }

    if (e->type == INPUT_BUTTON)
    {
        if (e->value != 1)
        {
            return 1;
        }

        if (!point_in(e->x, e->y, bar_box.x, bar_box.y, bar_box.w, bar_box.h))
        {
            bar_on = 0;
            damage_all();
            return 1;
        }

        int row_top = bar_box.y + 58 + 6;

        for (int i = 0, y = row_top; i < result_count; i++)
        {
            if (i == 0 || results[i].kind != results[i - 1].kind)
            {
                y += 26;
            }

            if (e->y >= y && e->y < y + 38 && e->x < bar_box.x + bar_box.w - SI(300))
            {
                activate(i, 0);
                return 1;
            }

            y += 38;
        }

        return 1;
    }

    if (e->type != INPUT_KEY)
    {
        return 1;
    }

    if (e->value == 0)
    {
        return e->code != KEY_SUPER;
    }

    int length = (int)strlen(query);

    switch (e->code)
    {
    case KEY_ESC:
        bar_on = 0;
        damage_all();
        return 1;
    case KEY_UP:
        if (selected > 0)
        {
            selected--;
        }

        break;
    case KEY_DOWN:
        if (selected + 1 < result_count)
        {
            selected++;
        }

        break;
    case KEY_TAB:
        scope = (scope + 1) % 4;
        knoc_bar_rebuild();
        return 1;
    case KEY_ENTER:
        activate(selected, (e->modifiers & INPUT_CTRL) ? 1 : (e->modifiers & INPUT_SHIFT) ? 2 : 0);
        return 1;
    case KEY_BACKSPACE:
        if (length)
        {
            query[length - 1] = 0;
            last_typed = uptime();
            knoc_bar_rebuild();
        }

        return 1;
    case KEY_SUPER:
        return 0;
    default:
        if (e->text >= 32 && e->text < 127 && length < (int)sizeof(query) - 1 && !(e->modifiers & (INPUT_CTRL | INPUT_SUPER)))
        {
            query[length] = (char)e->text;
            query[length + 1] = 0;
            selected = 0;
            last_typed = uptime();
            knoc_bar_rebuild();
            return 1;
        }

        return 0;
    }

    damage(bar_box.x, bar_box.y, bar_box.w, bar_box.h);
    return 1;
}

static const char *kind_group(int kind)
{
    switch (kind)
    {
    case RESULT_MEANING:
        return "Files · by meaning";
    case RESULT_FILE:
        return "Files · by name";
    case RESULT_APP:
        return "Apps";
    case RESULT_SETTING:
        return "Settings";
    case RESULT_WEB:
        return "Web";
    case RESULT_ASK:
        return "Ask";
    default:
        return "Actions";
    }
}

static void draw_preview(canvas_t *view, int x, int y, int w, int h)
{
    draw_fill(view, x, y, w, h, T.w2);

    if (selected >= result_count)
    {
        char lines[6][160];
        int n = wrap_lines(font_ui, S(12.5f), "Type to open apps and settings, find files by name and by meaning, ask Knoc or run a command.", w - 32, lines, 6);

        for (int k = 0; k < n; k++)
        {
            draw_text(view, font_ui, S(12.5f), x + 16, y + 30 + k * SI(19), lines[k], T.i3);
        }

        return;
    }

    result_t *r = &results[selected];

    if (r->kind == RESULT_FILE || r->kind == RESULT_MEANING)
    {
        const char *dot = strrchr(r->target, '.');
        int image = dot && (strcmp(dot, ".png") == 0 || strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0);
        int py = y + 16;

        if (image)
        {
            if (strcmp(preview_image_path, r->target) != 0)
            {
                if (preview_image_ok)
                {
                    image_free(&preview_image);
                }

                snprintf(preview_image_path, sizeof(preview_image_path), "%s", r->target);
                preview_image_ok = image_load(&preview_image, r->target) == 0;
            }

            if (preview_image_ok)
            {
                int tw = w - 32;
                int th = preview_image.height * tw / (preview_image.width ? preview_image.width : 1);

                th = th > 180 ? 180 : th;

                for (int j = 0; j < th; j++)
                {
                    for (int i = 0; i < tw; i++)
                    {
                        int sx = i * preview_image.width / tw;
                        int sy = j * preview_image.height / (th ? th : 1);
                        uint32_t p = preview_image.pixels[(long)sy * preview_image.width + sx];

                        draw_pixel(view, x + 16 + i, py + j, p & 0xFFFFFF, (int)(p >> 24));
                    }
                }

                py += th + 14;
            }
        }
        else
        {
            FILE *f = fopen(r->target, "r");
            int ph = 160;

            draw_round(view, x + 16, py, w - 32, ph, 3, 0xFCFCFA, 255);

            if (f)
            {
                char line[120];
                int k = 0;

                while (k < 9 && fgets(line, sizeof(line), f))
                {
                    char fitted[120];
                    int binary = 0;

                    line[strcspn(line, "\r\n")] = 0;

                    for (int i = 0; line[i]; i++)
                    {
                        binary |= (unsigned char)line[i] < 9;
                    }

                    if (binary)
                    {
                        draw_text(view, font_ui, S(11), x + 26, py + 22, "Not a text file", 0x6C737A);
                        break;
                    }

                    text_fit(fitted, sizeof(fitted), font_mono, S(9.5f), line, w - 54);
                    draw_text(view, font_mono, S(9.5f), x + 26, py + 20 + k * 15, fitted, 0x1A1C1E);
                    k++;
                }

                fclose(f);
            }

            py += ph + 14;
        }

        file_stat_t st;
        char where[200];

        snprintf(where, sizeof(where), "%s", r->target);

        char *slash = strrchr(where, '/');

        if (slash && slash != where)
        {
            *slash = 0;
        }

        draw_label(view, x + 16, py + 12, "Where", T.i3);
        draw_text(view, font_ui, S(11.5f), x + 84, py + 12, where, T.i2);

        if (stat(r->target, &st) == 0)
        {
            char size[32];

            snprintf(size, sizeof(size), "%lu bytes", (unsigned long)st.size);
            draw_label(view, x + 16, py + 32, "Size", T.i3);
            draw_text(view, font_ui, S(11.5f), x + 84, py + 32, size, T.i2);
        }

        draw_label(view, x + 16, py + 52, "Found", T.i3);
        draw_text(view, font_ui, S(11.5f), x + 84, py + 52, r->kind == RESULT_MEANING ? "by meaning (KnocEmbed)" : "by name", T.i2);
        return;
    }

    char lines[6][160];
    const char *text = r->kind == RESULT_WEB ? "Opens the KnocOS web browser in a Terminal window. It shows the page as text with numbered links: type a number to follow a link, s WORDS to search, u to go back, q to quit."
                       : r->kind == RESULT_ASK ? "Knoc answers in the Assist panel. Changes to your files show up there as a list you allow or deny."
                       : r->kind == RESULT_RUN ? "Opens a Terminal window and types this command into knocsh."
                                               : r->detail;
    int n = wrap_lines(font_ui, S(12.5f), text, w - 32, lines, 6);

    draw_text(view, font_bold, S(15), x + 16, y + 34, r->kind == RESULT_ASK ? "Ask Knoc" : r->kind == RESULT_RUN ? "Run" : r->kind == RESULT_WEB ? "Web" : r->title, T.i);

    for (int k = 0; k < n; k++)
    {
        draw_text(view, font_ui, S(12.5f), x + 16, y + 60 + k * SI(19), lines[k], T.i2);
    }
}

void knoc_bar_draw(canvas_t *view, int ox, int oy)
{
    if (!bar_on)
    {
        return;
    }

    uint32_t tint = (T.desk >> 1) & 0x7F7F7F;

    for (int j = 0; j < view->height; j++)
    {
        uint32_t *row = view->pixels + (long)j * view->stride;

        for (int i = 0; i < view->width; i++)
        {
            row[i] = ((row[i] >> 1) & 0x7F7F7F) + tint;
        }
    }

    int w = SI(820) > screen_w - 40 ? screen_w - 40 : SI(820);
    int h = 58 + SI(372) + 36;
    int bx = (screen_w - assist_width() - w) / 2;
    int by = 92;

    bar_box = (rect_t){bx, by, w, h};

    if (bx + w + 12 < ox || bx - 12 > ox + view->width || by + h + 16 < oy || by - 12 > oy + view->height)
    {
        return;
    }

    int x = bx - ox;
    int y = by - oy;

    for (int k = 0; k < 7; k++)
    {
        draw_frame(view, x - k - 1, y - k + 5, w + 2 * k + 2, h + 2 * k + 2, 10 + k, 0x000000, 40 - k * 5);
    }

    draw_round(view, x, y, w, h, 9, T.w, 255);
    draw_frame(view, x, y, w, h, 9, T.dark ? 0x000000 : 0x8A96A3, T.dark ? 150 : 70);
    draw_icon(view, ICON_SEARCH, x + 18, y + 21, 17, T.i3);

    int tw = draw_text(view, font_ui, S(19), x + 46, y + 36, query[0] ? query : "", T.i);

    if (!query[0])
    {
        draw_text(view, font_ui, S(19), x + 46, y + 36, "Open, find or ask", T.i3);
    }

    draw_fill(view, x + 47 + tw, y + 18, 2, 24, T.a);

    int sx = x + w - 18;

    for (int k = 3; k >= 0; k--)
    {
        int sw = text_width(font_ui, S(11), scopes[k]) + 16;

        sx -= sw + 4;
        draw_round(view, sx, y + 19, sw, 22, 4, k == scope ? T.w3 : T.w, 255);
        draw_frame(view, sx, y + 19, sw, 22, 4, k == scope ? T.ln2 : T.ln, 255);
        draw_text(view, font_ui, S(11), sx + 8, y + 34, scopes[k], k == scope ? T.i : T.i3);
    }

    draw_fill(view, x, y + 58, w, 1, T.ln);

    int pw = SI(300);
    int list_w = w - pw;
    int ry = y + 58 + 6;

    draw_fill(view, x + list_w, y + 59, 1, SI(372), T.ln);

    for (int i = 0; i < result_count && ry < y + 58 + SI(372) - 38; i++)
    {
        result_t *r = &results[i];

        if (i == 0 || r->kind != results[i - 1].kind)
        {
            draw_label(view, x + 16, ry + 18, kind_group(r->kind), T.i3);
            ry += 26;
        }

        if (i == selected)
        {
            draw_round(view, x + 6, ry, list_w - 12, 38, 5, T.a, T.dark ? 38 : 26);
        }

        if (r->kind == RESULT_FILE || r->kind == RESULT_MEANING)
        {
            const char *dot = strrchr(r->title, '.');
            char ext[6] = "FILE";

            if (dot && strlen(dot + 1) <= 4 && dot[1])
            {
                int n = 0;

                for (const char *p = dot + 1; *p && n < 4; p++)
                {
                    ext[n++] = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
                }

                ext[n] = 0;
            }

            draw_round(view, x + 16, ry + 11, 34, 15, 3, T.w, 255);
            draw_frame(view, x + 16, ry + 11, 34, 15, 3, T.ln2, 255);
            draw_text(view, font_label, S(8.5f), x + 33 - text_width(font_label, S(8.5f), ext) / 2, ry + 22, ext, T.i2);
        }
        else if (r->kind == RESULT_ASK)
        {
            presence_draw(view, x + 26, ry + 12, 0);
        }
        else
        {
            int icon = r->kind == RESULT_WEB ? ICON_NETWORK
                       : r->kind == RESULT_RUN ? ICON_TERMINAL
                       : r->app == &app_files ? ICON_FOLDER
                       : r->app == &app_terminal ? ICON_TERMINAL
                       : r->app == &app_monitor ? ICON_MONITOR
                       : r->app == &app_editor ? ICON_EDIT
                                               : ICON_SETTINGS;

            draw_icon(view, icon, x + 25, ry + 11, 15, T.i3);
        }

        char title[160];

        text_fit(title, sizeof(title), i == selected ? font_bold : font_ui, S(13), r->title, list_w - 150);
        draw_text(view, i == selected ? font_bold : font_ui, S(13), x + 60, ry + 17, title, T.i);
        text_fit(title, sizeof(title), font_ui, S(11), r->detail, list_w - 150);
        draw_text(view, font_ui, S(11), x + 60, ry + 32, title, T.i3);

        const char *keys = r->kind == RESULT_ASK ? "CTRL ENTER" : r->kind == RESULT_RUN ? "SHIFT ENTER" : i == selected ? "ENTER" : "";

        if (keys[0])
        {
            int kw = text_width(font_label, S(9), keys) + 10;

            draw_frame(view, x + list_w - kw - 16, ry + 11, kw, 16, 3, T.ln2, 255);
            draw_text(view, font_label, S(9), x + list_w - kw - 11, ry + 23, keys, T.i3);
        }

        ry += 38;
    }

    if (query[0] && strcmp(meaning_for, query) != 0 && (scope == 0 || scope == 1))
    {
        draw_text(view, font_ui, S(11.5f), x + 16, y + 58 + SI(372) - 12, "Searching by meaning…", T.i3);
    }

    draw_preview(view, x + list_w + 1, y + 59, pw - 2, SI(372) - 1);
    draw_fill(view, x, y + h - 36, w, 1, T.ln);

    static const char *const hints[][2] = {{"↑↓", "Move"}, {"ENTER", "Open"}, {"TAB", "Scope"}, {"ESC", "Close"}};
    int hx = x + 16;

    for (int k = 0; k < 4; k++)
    {
        int kw = text_width(font_label, S(9), hints[k][0]) + 10;

        draw_frame(view, hx, y + h - 26, kw, 16, 3, T.ln2, 255);
        draw_text(view, font_label, S(9), hx + 5, y + h - 14, hints[k][0], T.i3);
        hx += kw + 6;
        hx += draw_text(view, font_ui, S(11.5f), hx, y + h - 14, hints[k][1], T.i3) + 18;
    }

    char stats[64];

    snprintf(stats, sizeof(stats), "KnocEmbed · %d files by name", name_count);
    draw_text(view, font_mono, S(10.5f), x + w - 16 - text_width(font_mono, S(10.5f), stats), y + h - 14, stats, T.i3);
}
