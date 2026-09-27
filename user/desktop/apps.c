#include "desktop.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FILES_MAX 512
#define HISTORY_MAX 32

typedef struct file_row
{
    dir_entry_t entry;
    char placed[40];
    int placed_known;
} file_row_t;

typedef struct files
{
    char path[256];
    char history[HISTORY_MAX][256];
    int history_count;
    file_row_t *rows;
    int count;
    int shown[FILES_MAX];
    int shown_count;
    int selected;
    int scroll;
    char query[96];
    int query_focus;
    int meaning;
    int meaning_count;
    char meaning_paths[24][160];
    int confirm_delete;
    uint64_t last_click;
    int last_row;
} files_t;

static const struct
{
    const char *label;
    const char *path;
    int icon;
} places[] = {
    {"Home", "/home", ICON_HOME},
    {"Downloads", "/home/Downloads", ICON_DOWNLOAD},
    {"Documents", "/home/Documents", ICON_DOC},
    {"Pictures", "/home/Pictures", ICON_PICTURE},
    {"Programs", "/bin", ICON_TERMINAL},
    {"Whole disk", "/", ICON_DISK},
};

#define PLACES (int)(sizeof(places) / sizeof(places[0]))
#define SIDEBAR 168
#define TOOLBAR 42
#define HEADER 30
#define ROW 30
#define STATUS 26

static void join(char *out, int size, const char *dir, const char *name)
{
    if (strcmp(dir, "/") == 0)
    {
        snprintf(out, (size_t)size, "/%s", name);
    }
    else
    {
        snprintf(out, (size_t)size, "%s/%s", dir, name);
    }
}

static int compare_rows(const void *a, const void *b)
{
    const file_row_t *x = a;
    const file_row_t *y = b;
    int dx = x->entry.type == 2;
    int dy = y->entry.type == 2;

    if (dx != dy)
    {
        return dy - dx;
    }

    return strcmp(x->entry.name, y->entry.name);
}

static int contains(const char *text, const char *part)
{
    if (!part[0])
    {
        return 1;
    }

    for (; *text; text++)
    {
        int i = 0;

        while (part[i] && (text[i] | 0x20) == (part[i] | 0x20))
        {
            i++;
        }

        if (!part[i])
        {
            return 1;
        }
    }

    return 0;
}

static void filter(files_t *f)
{
    f->shown_count = 0;

    for (int i = 0; i < f->count; i++)
    {
        if (f->meaning || contains(f->rows[i].entry.name, f->query))
        {
            f->shown[f->shown_count++] = i;
        }
    }

    if (f->selected >= f->shown_count)
    {
        f->selected = f->shown_count - 1;
    }
}

static void load(window_t *w, files_t *f)
{
    f->count = 0;
    f->meaning = 0;

    for (unsigned long i = 0; f->count < FILES_MAX; i++)
    {
        if (readdir(f->path, i, &f->rows[f->count].entry) != 0)
        {
            break;
        }

        f->rows[f->count].placed_known = 0;
        f->count++;
    }

    qsort(f->rows, (size_t)f->count, sizeof(file_row_t), compare_rows);
    f->selected = f->count ? 0 : -1;
    f->scroll = 0;
    f->confirm_delete = 0;
    filter(f);
    window_set_title(w, "Files", f->path);
    window_dirty(w);
}

static void navigate(window_t *w, files_t *f, const char *path)
{
    if (strcmp(path, f->path) != 0 && f->history_count < HISTORY_MAX)
    {
        snprintf(f->history[f->history_count++], 256, "%s", f->path);
    }

    snprintf(f->path, sizeof(f->path), "%s", path);
    f->query[0] = 0;
    load(w, f);
}

static void search_meaning(window_t *w, files_t *f)
{
    char args[128];

    snprintf(args, sizeof(args), "%s", f->query);

    int pid = spawn_capture("find", args, 2);

    if (pid < 0)
    {
        notify("Files", "Search by meaning is not available", "The find program could not start");
        return;
    }

    wait(pid);

    char out[4096];
    long n = syscall(SYS_CAPTURED, (long)out, sizeof(out) - 1, 0);

    out[n > 0 ? n : 0] = 0;
    f->count = 0;
    f->meaning = 1;

    char *line = strtok(out, "\n");

    while (line && f->count < 24)
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

            file_stat_t st;

            if (stat(path, &st) == 0)
            {
                file_row_t *row = &f->rows[f->count];

                memset(row, 0, sizeof(*row));
                snprintf(row->entry.name, sizeof(row->entry.name), "%s", path);
                row->entry.type = st.type;
                row->entry.size = st.size;
                row->entry.modified = st.modified;
                f->count++;
            }
        }

        line = strtok(0, "\n");
    }

    f->selected = f->count ? 0 : -1;
    f->scroll = 0;
    filter(f);
    window_set_title(w, "Files", "search by meaning");
    window_dirty(w);
}

static void placed_by(files_t *f, file_row_t *row)
{
    char path[256];
    graph_request_t request;
    graph_edge_info_t edge;

    row->placed_known = 1;
    row->placed[0] = 0;

    if (f->meaning)
    {
        snprintf(path, sizeof(path), "%s", row->entry.name);
    }
    else
    {
        join(path, sizeof(path), f->path, row->entry.name);
    }

    for (unsigned i = 0; i < 20; i++)
    {
        memset(&request, 0, sizeof(request));
        request.op = GRAPH_OP_EDGES;
        request.index = i;
        request.kind_a = GRAPH_KIND_FILE;
        snprintf(request.a, sizeof(request.a), "%s", path);

        if (graph(&request, &edge) != 0)
        {
            break;
        }

        if (edge.relation == GRAPH_REL_MOVED_TO && strcmp(edge.to, path) == 0)
        {
            if (strstr(edge.actor, "organiz"))
            {
                snprintf(row->placed, sizeof(row->placed), "Knoc · organizer");
            }
            else
            {
                snprintf(row->placed, sizeof(row->placed), "You");
            }

            return;
        }

        if (edge.relation == GRAPH_REL_CAME_FROM && !row->placed[0])
        {
            snprintf(row->placed, sizeof(row->placed), "Downloaded");
        }
    }
}

static void size_text(char *out, int size, uint64_t bytes, int directory)
{
    if (directory)
    {
        snprintf(out, (size_t)size, "—");
    }
    else if (bytes < 1024)
    {
        snprintf(out, (size_t)size, "%lu B", (unsigned long)bytes);
    }
    else if (bytes < 1024 * 1024)
    {
        snprintf(out, (size_t)size, "%lu KB", (unsigned long)(bytes / 1024));
    }
    else if (bytes < 1024UL * 1024 * 1024)
    {
        snprintf(out, (size_t)size, "%lu.%lu MB", (unsigned long)(bytes >> 20), (unsigned long)((bytes >> 20) % 10));
    }
    else
    {
        snprintf(out, (size_t)size, "%lu GB", (unsigned long)(bytes >> 30));
    }
}

static void date_text(char *out, int size, uint64_t seconds)
{
    static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    if (seconds == 0)
    {
        snprintf(out, (size_t)size, "—");
        return;
    }

    time_t t = (time_t)seconds;
    struct tm *tm = gmtime(&t);

    snprintf(out, (size_t)size, "%02d %s", tm->tm_mday, months[tm->tm_mon % 12]);
}

static const char *extension_label(const char *name, int type)
{
    static char label[6];

    if (type == 2)
    {
        return "DIR";
    }

    const char *dot = strrchr(name, '.');

    if (!dot || !dot[1] || strlen(dot + 1) > 4)
    {
        return type == 3 ? "LINK" : "FILE";
    }

    int n = 0;

    for (const char *p = dot + 1; *p && n < 4; p++)
    {
        label[n++] = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
    }

    label[n] = 0;
    return label;
}

static int visible_rows(window_t *w)
{
    return (w->content.height - TOOLBAR - HEADER - STATUS) / ROW;
}

static void files_draw(window_t *w, canvas_t *c)
{
    files_t *f = w->state;
    int width = c->width;
    int height = c->height;

    draw_fill(c, 0, 0, width, height, T.w);
    draw_fill(c, 0, TOOLBAR - 1, width, 1, T.ln);

    draw_icon(c, ICON_BACK, 14, 13, 14, f->history_count ? T.i2 : T.ln2);
    draw_icon(c, ICON_FORWARD, 40, 13, 14, T.ln2);

    int qx = 70;
    int qw = width - qx - 12;

    ui_field(c, qx, 7, qw, 28, f->query, "Filter by name, or switch to Meaning", f->query_focus);

    int mode_w = text_width(font_ui, S(11), "Name") + text_width(font_ui, S(11), "Meaning") + 32;
    int mx = qx + qw - mode_w - 5;

    draw_round(c, mx, 11, mode_w, 20, 4, T.w, 255);
    draw_frame(c, mx, 11, mode_w, 20, 4, T.ln, 255);

    int nw = text_width(font_ui, S(11), "Name") + 16;

    draw_round(c, f->meaning ? mx + nw : mx, 11, f->meaning ? mode_w - nw : nw, 20, 4, T.w3, 255);
    draw_fill(c, f->meaning ? mx + nw + 2 : mx + 2, 29, (f->meaning ? mode_w - nw : nw) - 4, 2, T.a);
    draw_text(c, font_ui, S(11), mx + 8, 25, "Name", f->meaning ? T.i3 : T.i);
    draw_text(c, font_ui, S(11), mx + nw + 8, 25, "Meaning", f->meaning ? T.i : T.i3);

    draw_fill(c, SIDEBAR, TOOLBAR, 1, height - TOOLBAR, T.ln);
    draw_label(c, 16, TOOLBAR + 24, "Places", T.i3);

    for (int i = 0; i < PLACES; i++)
    {
        int y = TOOLBAR + 34 + i * 28;
        int on = !f->meaning && strcmp(f->path, places[i].path) == 0;

        if (on)
        {
            draw_round(c, 8, y, SIDEBAR - 16, 26, 4, T.w3, 255);
        }

        draw_icon(c, places[i].icon, 16, y + 6, 14, on ? T.a : T.i3);
        draw_text(c, on ? font_bold : font_ui, S(13), 40, y + 18, places[i].label, on ? T.i : T.i2);
    }

    int lx = SIDEBAR + 1;
    int lw = width - lx;
    int size_x = lx + lw - 108 - 70 - 64 - 14;
    int date_x = size_x + 70 + 12;
    int placed_x = date_x + 64 + 12;

    draw_fill(c, lx, TOOLBAR + HEADER - 1, lw, 1, T.ln);
    draw_label(c, lx + 14, TOOLBAR + 20, "Name", T.i3);
    draw_label(c, size_x + 10, TOOLBAR + 20, "Size", T.i3);
    draw_label(c, date_x, TOOLBAR + 20, "Modified", T.i3);
    draw_label(c, placed_x, TOOLBAR + 20, "Placed by", T.i3);

    int rows = visible_rows(w);

    for (int k = 0; k < rows && f->scroll + k < f->shown_count; k++)
    {
        int index = f->scroll + k;
        file_row_t *row = &f->rows[f->shown[index]];
        int y = TOOLBAR + HEADER + k * ROW;
        int selected = index == f->selected;

        if (selected)
        {
            draw_blend(c, lx, y, lw, ROW, T.a, T.dark ? 38 : 26);
        }

        draw_fill(c, lx + 8, y + ROW - 1, lw - 16, 1, T.dark ? 0x23272B : 0xECEEF0);

        const char *ext = extension_label(row->entry.name, (int)row->entry.type);

        draw_round(c, lx + 14, y + 8, 34, 15, 3, T.w, 255);
        draw_frame(c, lx + 14, y + 8, 34, 15, 3, T.ln2, 255);

        int ew = text_width(font_label, S(8.5f), ext);

        draw_text(c, font_label, S(8.5f), lx + 31 - ew / 2, y + 19, ext, T.i2);

        char name[160];

        text_fit(name, sizeof(name), selected ? font_bold : font_ui, S(13), row->entry.name, size_x - lx - 70);
        draw_text(c, selected ? font_bold : font_ui, S(13), lx + 58, y + 20, name, T.i);

        char text[32];

        size_text(text, sizeof(text), row->entry.size, row->entry.type == 2);
        draw_text(c, font_mono, S(11.5f), size_x + 70 - text_width(font_mono, S(11.5f), text), y + 20, text, T.i2);
        date_text(text, sizeof(text), row->entry.modified);
        draw_text(c, font_mono, S(11.5f), date_x, y + 20, text, T.i2);

        if (!row->placed_known)
        {
            placed_by(f, row);
        }

        if (row->placed[0])
        {
            int ai = row->placed[0] == 'K';

            draw_round(c, placed_x, y + 13, 5, 5, 1, ai ? T.a : row->placed[0] == 'Y' ? T.ok : T.i3, 255);
            draw_text(c, font_ui, S(11.5f), placed_x + 11, y + 20, row->placed, ai ? T.i2 : T.i3);
        }
    }

    if (f->shown_count == 0)
    {
        draw_text(c, font_ui, S(13), lx + 20, TOOLBAR + HEADER + 30,
                  f->meaning ? "Nothing matched that meaning." : "This folder is empty.", T.i3);
    }

    draw_fill(c, lx, height - STATUS, lw, 1, T.ln);

    char status[200];

    if (f->confirm_delete && f->selected >= 0)
    {
        snprintf(status, sizeof(status), "Delete %s? Press Delete again to delete it, Esc to keep it.",
                 f->rows[f->shown[f->selected]].entry.name);
        draw_text(c, font_mono, S(10.5f), lx + 14, height - 9, status, T.need);
    }
    else
    {
        snprintf(status, sizeof(status), "%d item%s%s", f->shown_count, f->shown_count == 1 ? "" : "s",
                 f->meaning ? " found by meaning (KnocEmbed)" : "");
        draw_text(c, font_mono, S(10.5f), lx + 14, height - 9, status, T.i3);
    }
}

static void open_selected(window_t *w, files_t *f)
{
    if (f->selected < 0 || f->selected >= f->shown_count)
    {
        return;
    }

    file_row_t *row = &f->rows[f->shown[f->selected]];
    char path[256];

    if (f->meaning)
    {
        snprintf(path, sizeof(path), "%s", row->entry.name);
    }
    else
    {
        join(path, sizeof(path), f->path, row->entry.name);
    }

    if (row->entry.type == 2)
    {
        navigate(w, f, path);
    }
    else
    {
        open_path(path);
    }
}

static void ensure_visible(window_t *w, files_t *f)
{
    int rows = visible_rows(w);

    if (f->selected < f->scroll)
    {
        f->scroll = f->selected;
    }

    if (f->selected >= f->scroll + rows)
    {
        f->scroll = f->selected - rows + 1;
    }

    if (f->scroll < 0)
    {
        f->scroll = 0;
    }
}

static void parent(window_t *w, files_t *f)
{
    char path[256];

    snprintf(path, sizeof(path), "%s", f->path);

    char *slash = strrchr(path, '/');

    if (slash == path)
    {
        path[1] = 0;
    }
    else if (slash)
    {
        *slash = 0;
    }

    navigate(w, f, path);
}

static void files_event(window_t *w, const input_event_t *e, int x, int y)
{
    files_t *f = w->state;

    if (e->type == INPUT_WHEEL)
    {
        f->scroll -= e->value * 3;

        if (f->scroll > f->shown_count - visible_rows(w))
        {
            f->scroll = f->shown_count - visible_rows(w);
        }

        if (f->scroll < 0)
        {
            f->scroll = 0;
        }

        window_dirty(w);
        return;
    }

    if (e->type == INPUT_BUTTON && e->value == 1 && e->code == BUTTON_LEFT)
    {
        int width = w->content.width;
        int qx = 70;
        int qw = width - qx - 12;
        int mode_w = text_width(font_ui, S(11), "Name") + text_width(font_ui, S(11), "Meaning") + 32;
        int mx = qx + qw - mode_w - 5;
        int nw = text_width(font_ui, S(11), "Name") + 16;

        f->confirm_delete = 0;

        if (point_in(x, y, 8, 6, 28, 30) && f->history_count)
        {
            f->history_count--;
            snprintf(f->path, sizeof(f->path), "%s", f->history[f->history_count]);
            load(w, f);
            return;
        }

        if (point_in(x, y, mx, 9, mode_w, 24))
        {
            int want = x >= mx + nw;

            if (want && f->query[0])
            {
                search_meaning(w, f);
            }
            else if (!want && f->meaning)
            {
                load(w, f);
            }

            f->query_focus = 1;
            window_dirty(w);
            return;
        }

        f->query_focus = point_in(x, y, qx, 7, qw, 28);

        if (x < SIDEBAR && y > TOOLBAR + 34)
        {
            int i = (y - TOOLBAR - 34) / 28;

            if (i >= 0 && i < PLACES)
            {
                mkdir(places[i].path);
                navigate(w, f, places[i].path);
            }

            return;
        }

        if (x > SIDEBAR && y > TOOLBAR + HEADER && y < w->content.height - STATUS)
        {
            int index = f->scroll + (y - TOOLBAR - HEADER) / ROW;

            if (index < f->shown_count)
            {
                uint64_t now = uptime();

                if (index == f->last_row && now - f->last_click < 40)
                {
                    f->selected = index;
                    open_selected(w, f);
                    f->last_click = 0;
                    return;
                }

                f->selected = index;
                f->last_row = index;
                f->last_click = now;
            }
        }

        window_dirty(w);
        return;
    }

    if (e->type != INPUT_KEY || e->value == 0)
    {
        return;
    }

    if (f->query_focus && e->code != KEY_UP && e->code != KEY_DOWN)
    {
        int length = (int)strlen(f->query);

        if (e->code == KEY_BACKSPACE)
        {
            if (length)
            {
                f->query[length - 1] = 0;
            }
        }
        else if (e->code == KEY_ENTER)
        {
            if (f->meaning)
            {
                search_meaning(w, f);
            }
            else
            {
                open_selected(w, f);
            }

            return;
        }
        else if (e->code == KEY_ESC)
        {
            f->query[0] = 0;
            f->query_focus = 0;
        }
        else if (e->text >= 32 && e->text < 127 && length < (int)sizeof(f->query) - 1)
        {
            f->query[length] = (char)e->text;
            f->query[length + 1] = 0;
        }

        if (!f->meaning)
        {
            filter(f);
        }

        window_dirty(w);
        return;
    }

    switch (e->code)
    {
    case KEY_UP:
        if (f->selected > 0)
        {
            f->selected--;
        }

        break;
    case KEY_DOWN:
        if (f->selected + 1 < f->shown_count)
        {
            f->selected++;
        }

        break;
    case KEY_ENTER:
        open_selected(w, f);
        return;
    case KEY_BACKSPACE:
        parent(w, f);
        return;
    case KEY_ESC:
        f->confirm_delete = 0;
        break;
    case KEY_DELETE:
        if (f->selected >= 0 && !f->meaning)
        {
            if (!f->confirm_delete)
            {
                f->confirm_delete = 1;
            }
            else
            {
                char path[256];

                join(path, sizeof(path), f->path, f->rows[f->shown[f->selected]].entry.name);

                if (remove(path) == 0)
                {
                    notify("Files", "Deleted", path);
                }
                else
                {
                    notify("Files", "Could not delete", path);
                }

                load(w, f);
                return;
            }
        }

        break;
    default:
        if (e->text >= 32 && e->text < 127)
        {
            f->query_focus = 1;
            f->query[0] = (char)e->text;
            f->query[1] = 0;
            filter(f);
        }

        break;
    }

    ensure_visible(w, f);
    window_dirty(w);
}

static int files_open(window_t *w, const char *arg)
{
    files_t *f = calloc(1, sizeof(files_t));

    if (!f)
    {
        return -1;
    }

    f->rows = calloc(FILES_MAX, sizeof(file_row_t));

    if (!f->rows)
    {
        free(f);
        return -1;
    }

    w->state = f;
    snprintf(f->path, sizeof(f->path), "%s", arg && arg[0] ? arg : "/home");
    f->last_row = -1;
    load(w, f);
    return 0;
}

static void files_close(window_t *w)
{
    files_t *f = w->state;

    free(f->rows);
    free(f);
}

const app_t app_files = {"Files", 780, 500, files_open, files_draw, files_event, 0, files_close};

typedef struct settings
{
    int section;
    canvas_t thumbs[WALL_COUNT];
    int thumbs_theme;
    int thumbs_accent;
} settings_t;

static const char *const sections[] = {"Appearance", "Display", "About"};

static void settings_thumbs(settings_t *s)
{
    if (s->thumbs_theme == T.dark && s->thumbs_accent == T.accent_index && s->thumbs[0].pixels)
    {
        return;
    }

    for (int i = 0; i < WALL_COUNT; i++)
    {
        if (!s->thumbs[i].pixels)
        {
            canvas_create(&s->thumbs[i], 128, 80);
        }

        if (s->thumbs[i].pixels)
        {
            wallpaper_draw(&s->thumbs[i], i, T.dark ? THEME_GRAPHITE : THEME_PAPER, T.accent_index);
        }
    }

    s->thumbs_theme = T.dark;
    s->thumbs_accent = T.accent_index;
}

static void settings_draw(window_t *w, canvas_t *c)
{
    settings_t *s = w->state;

    draw_fill(c, 0, 0, c->width, c->height, T.w);
    draw_fill(c, 156, 0, 1, c->height, T.ln);

    static const int icons[] = {ICON_SETTINGS, ICON_MONITOR, ICON_DOC};

    for (int i = 0; i < 3; i++)
    {
        int y = 10 + i * 28;
        int on = i == s->section;

        if (on)
        {
            draw_round(c, 6, y, 144, 26, 4, T.w3, 255);
        }

        draw_icon(c, icons[i], 14, y + 6, 14, on ? T.a : T.i3);
        draw_text(c, on ? font_bold : font_ui, S(13), 38, y + 18, sections[i], on ? T.i : T.i2);
    }

    int x = 178;
    int y = 34;

    draw_text(c, font_bold, S(16), x, y, sections[s->section], T.i);
    y += 30;

    if (s->section == 0)
    {
        draw_label(c, x, y + 12, "Theme", T.i3);

        for (int k = 0; k < 2; k++)
        {
            int tx = x + 100 + k * 128;
            int on = (k == 0) == (T.dark != 0);
            uint32_t bg = k == 0 ? 0x121416 : 0xDEE1E5;
            uint32_t card = k == 0 ? 0x1F2226 : 0xFBFBFC;
            uint32_t line = k == 0 ? 0xECEDEE : 0x111418;

            draw_round(c, tx, y, 118, 64, 5, bg, 255);
            draw_round(c, tx + 12, y + 12, 62, 40, 3, card, 255);
            draw_round(c, tx + 20, y + 22, 34, 4, 2, line, 255);
            draw_frame(c, tx, y, 118, 64, 5, on ? T.a : T.ln2, 255);

            if (on)
            {
                draw_frame(c, tx + 1, y + 1, 116, 62, 4, T.a, 255);
            }

            draw_text(c, on ? font_bold : font_ui, S(12), tx, y + 82, k == 0 ? "Graphite" : "Paper", on ? T.i : T.i2);
        }

        y += 104;
        draw_label(c, x, y + 16, "Accent", T.i3);

        for (int k = 0; k < ACCENT_COUNT; k++)
        {
            int cx = x + 113 + k * 58;
            int on = k == T.accent_index;

            if (on)
            {
                draw_circle(c, (float)cx, (float)(y + 13), 16.5f, T.i, 255);
                draw_circle(c, (float)cx, (float)(y + 13), 15.0f, T.w, 255);
            }

            draw_circle(c, (float)cx, (float)(y + 13), 13.0f, accent_color(k, T.dark ? THEME_GRAPHITE : THEME_PAPER), 255);

            char name[16];

            snprintf(name, sizeof(name), "%s", accent_name(k));
            name[0] = (char)(name[0] - 32);

            int nw = text_width(on ? font_bold : font_ui, S(11), name);

            draw_text(c, on ? font_bold : font_ui, S(11), cx - nw / 2, y + 46, name, on ? T.i : T.i3);
        }

        y += 72;
        draw_label(c, x, y + 16, "Wallpaper", T.i3);
        settings_thumbs(s);

        for (int k = 0; k < WALL_COUNT; k++)
        {
            int tx = x + 100 + k * 140;
            int on = k == T.wall;

            if (s->thumbs[k].pixels)
            {
                draw_canvas(c, &s->thumbs[k], tx, y);
            }

            draw_frame(c, tx - 1, y - 1, 130, 82, 4, on ? T.a : T.ln2, 255);

            if (on)
            {
                draw_frame(c, tx - 2, y - 2, 132, 84, 5, T.a, 255);
            }

            char name[16];

            snprintf(name, sizeof(name), "%s", wallpaper_name(k));
            name[0] = (char)(name[0] - 32);
            draw_text(c, on ? font_bold : font_ui, S(11), tx, y + 98, name, on ? T.i : T.i3);
        }

        y += 120;
        draw_text(c, font_ui, S(11.5f), x, y,
                  "The accent only marks things: selection, focus and Knoc's four squares. Text stays white or black.",
                  T.i3);
    }
    else if (s->section == 1)
    {
        char text[80];

        draw_label(c, x, y + 12, "Text size", T.i3);

        static const int sizes[] = {100, 125, 150, 175, 200};

        for (int k = 0; k < 5; k++)
        {
            int bx = x + 100 + k * 70;
            int on = sizes[k] == T.scale;

            snprintf(text, sizeof(text), "%d%%", sizes[k]);
            draw_round(c, bx, y - 4, 62, 28, 4, on ? T.i : T.w, 255);

            if (!on)
            {
                draw_frame(c, bx, y - 4, 62, 28, 4, T.ln2, 255);
            }

            draw_text(c, font_bold, S(12), bx + 31 - text_width(font_bold, S(12), text) / 2, y + 15, text, on ? T.w : T.i);
        }

        y += 56;
        draw_label(c, x, y, "Screen", T.i3);
        snprintf(text, sizeof(text), "%d × %d pixels, 32-bit colour", screen_w, screen_h);
        draw_text(c, font_ui, S(13), x + 100, y, text, T.i);
        y += 30;
        draw_label(c, x, y, "Pointer", T.i3);
        draw_text(c, font_ui, S(13), x + 100, y, "Drawn by the screen hardware, moved by the kernel", T.i);
    }
    else
    {
        system_info_t info;
        char text[128];

        sysinfo(&info);
        draw_label(c, x, y, "Version", T.i3);
        draw_text(c, font_ui, S(13), x + 100, y, "KnocOS 0.38.0", T.i);
        y += 28;
        draw_label(c, x, y, "Cores", T.i3);
        draw_text(c, font_ui, S(13), x + 100, y, "4 kernel cores, the AI space and 3 AI cores", T.i);
        y += 28;
        draw_label(c, x, y, "Memory", T.i3);
        snprintf(text, sizeof(text), "%lu MiB, %lu MiB free", (unsigned long)(info.ram_bytes >> 20),
                 (unsigned long)(info.ram_free_bytes >> 20));
        draw_text(c, font_ui, S(13), x + 100, y, text, T.i);
        y += 28;
        draw_label(c, x, y, "Disk", T.i3);
        snprintf(text, sizeof(text), "%lu MiB, %lu MiB free, %u files", (unsigned long)(info.disk_bytes >> 20),
                 (unsigned long)(info.disk_free_bytes >> 20), info.disk_files);
        draw_text(c, font_ui, S(13), x + 100, y, text, T.i);
        y += 28;
        draw_label(c, x, y, "Uptime", T.i3);
        snprintf(text, sizeof(text), "%lu minutes", (unsigned long)(info.uptime_ticks / 6000));
        draw_text(c, font_ui, S(13), x + 100, y, text, T.i);
        y += 40;
        draw_text(c, font_ui, S(11.5f), x, y, "Fonts: Hanken Grotesk, Martian Mono and JetBrains Mono (SIL Open Font License).", T.i3);
    }
}

static void settings_event(window_t *w, const input_event_t *e, int x, int y)
{
    settings_t *s = w->state;

    if (e->type != INPUT_BUTTON || e->value != 1)
    {
        return;
    }

    if (x < 156)
    {
        int i = (y - 10) / 28;

        if (i >= 0 && i < 3)
        {
            s->section = i;
            window_dirty(w);
        }

        return;
    }

    int bx = 178;
    int top = 64;

    if (s->section == 0)
    {
        for (int k = 0; k < 2; k++)
        {
            if (point_in(x, y, bx + 100 + k * 128, top, 118, 90))
            {
                theme_apply(k == 0, T.accent_index, T.wall, T.scale);
                settings_save();
                desktop_log(k == 0 ? "theme graphite" : "theme paper");
                return;
            }
        }

        for (int k = 0; k < ACCENT_COUNT; k++)
        {
            if (point_in(x, y, bx + 113 + k * 58 - 18, top + 104 - 6, 36, 56))
            {
                theme_apply(T.dark, k, T.wall, T.scale);
                settings_save();

                char text[48];

                snprintf(text, sizeof(text), "accent %s", accent_name(k));
                desktop_log(text);
                return;
            }
        }

        for (int k = 0; k < WALL_COUNT; k++)
        {
            if (point_in(x, y, bx + 100 + k * 140, top + 176, 130, 104))
            {
                theme_apply(T.dark, T.accent_index, k, T.scale);
                settings_save();

                char text[48];

                snprintf(text, sizeof(text), "wallpaper %s", wallpaper_name(k));
                desktop_log(text);
                return;
            }
        }
    }
    else if (s->section == 1)
    {
        static const int sizes[] = {100, 125, 150, 175, 200};

        for (int k = 0; k < 5; k++)
        {
            if (point_in(x, y, bx + 100 + k * 70, top - 4, 62, 28))
            {
                theme_apply(T.dark, T.accent_index, T.wall, sizes[k]);
                settings_save();
                return;
            }
        }
    }
}

static int settings_open(window_t *w, const char *arg)
{
    settings_t *s = calloc(1, sizeof(settings_t));

    if (!s)
    {
        return -1;
    }

    s->thumbs_theme = -1;
    s->section = arg && strcmp(arg, "about") == 0 ? 2 : arg && strcmp(arg, "display") == 0 ? 1 : 0;
    w->state = s;
    window_set_title(w, "Settings", s->section == 0 ? "settings / appearance" : s->section == 1 ? "settings / display" : "settings / about");
    return 0;
}

static void settings_close(window_t *w)
{
    settings_t *s = w->state;

    for (int i = 0; i < WALL_COUNT; i++)
    {
        canvas_free(&s->thumbs[i]);
    }

    free(s);
}

const app_t app_settings = {"Settings", 880, 540, settings_open, settings_draw, settings_event, 0, settings_close};

typedef struct monitor
{
    process_info_t procs[48];
    int proc_count;
    cpu_info_t cores[8];
    uint64_t busy[8];
    uint64_t idle[8];
    int load[8];
    system_info_t info;
    uint64_t last;
    int selected;
} monitor_t;

static void monitor_sample(monitor_t *m)
{
    for (int k = 0; k < 8; k++)
    {
        cpu_info_t info;

        if (cpuinfo((unsigned long)k, &info) != 0)
        {
            continue;
        }

        uint64_t busy = info.busy_ticks - m->busy[k];
        uint64_t idle = info.idle_ticks - m->idle[k];

        m->busy[k] = info.busy_ticks;
        m->idle[k] = info.idle_ticks;
        m->load[k] = busy + idle ? (int)(busy * 100 / (busy + idle)) : 0;
        m->cores[k] = info;
    }

    m->proc_count = 0;

    for (unsigned long i = 0; m->proc_count < 48 && ps(i, &m->procs[m->proc_count]) == 0; i++)
    {
        if (m->procs[m->proc_count].pid > 0)
        {
            m->proc_count++;
        }
    }

    sysinfo(&m->info);
}

static void monitor_draw(window_t *w, canvas_t *c)
{
    monitor_t *m = w->state;
    char text[96];

    draw_fill(c, 0, 0, c->width, c->height, T.w);
    draw_label(c, 18, 26, "Cores", T.i3);

    for (int k = 0; k < 8; k++)
    {
        int x = 18 + k * ((c->width - 36) / 8);
        int bw = (c->width - 36) / 8 - 10;
        int h = 60;
        int fill = m->load[k] * h / 100;

        draw_round(c, x, 36, bw, h, 4, T.w3, 255);

        if (fill > 0)
        {
            draw_round(c, x, 36 + h - fill, bw, fill, 4, k == 4 ? T.need : k >= 5 ? T.a : T.i2, 255);
        }

        snprintf(text, sizeof(text), "%d", k);
        draw_text(c, font_label, S(9.5f), x, 112, text, T.i3);
        snprintf(text, sizeof(text), "%s", k < 4 ? "KERNEL" : k == 4 ? "AI SPACE" : "AI");
        draw_text(c, font_label, S(8.5f), x + 12, 112, text, T.i3);
        snprintf(text, sizeof(text), "%d%%", m->load[k]);
        draw_text(c, font_mono, S(11), x, 128, text, T.i2);
    }

    unsigned long total = (unsigned long)(m->info.ram_bytes >> 20);
    unsigned long free_mb = (unsigned long)(m->info.ram_free_bytes >> 20);
    int bar = c->width - 36 - 120;

    draw_label(c, 18, 160, "Memory", T.i3);
    draw_round(c, 120, 150, bar, 10, 3, T.w3, 255);

    if (total)
    {
        draw_round(c, 120, 150, (int)((total - free_mb) * (unsigned long)bar / total), 10, 3, T.i2, 255);
    }

    snprintf(text, sizeof(text), "%lu of %lu MiB used", total - free_mb, total);
    draw_text(c, font_mono, S(11), 120, 178, text, T.i2);

    int y = 204;

    draw_fill(c, 0, y, c->width, 1, T.ln);
    draw_label(c, 18, y + 20, "PID", T.i3);
    draw_label(c, 70, y + 20, "Program", T.i3);
    draw_label(c, 230, y + 20, "Class", T.i3);
    draw_label(c, 340, y + 20, "State", T.i3);
    draw_label(c, 440, y + 20, "CPU", T.i3);
    draw_label(c, 520, y + 20, "Memory", T.i3);
    y += 30;

    static const char *const classes[] = {"interactive", "ai agent", "normal", "background", "idle"};
    static const char *const states[] = {"-", "ready", "running", "sleeping", "exited", "crashed", "loading", "waiting"};

    for (int i = 0; i < m->proc_count && y < c->height - 40; i++)
    {
        process_info_t *p = &m->procs[i];

        if (i == m->selected)
        {
            draw_blend(c, 0, y - 18, c->width, 26, T.a, T.dark ? 38 : 26);
        }

        snprintf(text, sizeof(text), "%d", p->pid);
        draw_text(c, font_mono, S(11.5f), 18, y, text, T.i2);
        draw_text(c, font_ui, S(13), 70, y, p->name, T.i);
        draw_text(c, font_ui, S(12), 230, y, classes[p->process_class % 5], T.i2);
        draw_text(c, font_ui, S(12), 340, y, states[p->state % 8], p->state == 5 ? T.bad : T.i2);
        snprintf(text, sizeof(text), "%lu.%02lus", (unsigned long)(p->cpu_ticks / 100), (unsigned long)(p->cpu_ticks % 100));
        draw_text(c, font_mono, S(11.5f), 440, y, text, T.i2);
        snprintf(text, sizeof(text), "%lu KB", (unsigned long)(p->memory / 1024));
        draw_text(c, font_mono, S(11.5f), 520, y, text, T.i2);
        y += 26;
    }

    if (m->selected >= 0 && m->selected < m->proc_count && m->procs[m->selected].user)
    {
        ui_button(c, c->width - 110, c->height - 38, "Stop program", 0);
    }
}

static void monitor_event(window_t *w, const input_event_t *e, int x, int y)
{
    monitor_t *m = w->state;

    if (e->type != INPUT_BUTTON || e->value != 1)
    {
        return;
    }

    if (y >= w->content.height - 38 && x >= w->content.width - 110 && m->selected >= 0 &&
        m->selected < m->proc_count && m->procs[m->selected].user)
    {
        process_info_t *p = &m->procs[m->selected];
        char text[80];

        if (kill(p->pid) == 0)
        {
            snprintf(text, sizeof(text), "%s (pid %d)", p->name, p->pid);
            notify("Monitor", "Stopped a program", text);
        }

        m->selected = -1;
        monitor_sample(m);
        window_dirty(w);
        return;
    }

    if (y > 216)
    {
        m->selected = (y - 216) / 26;
        window_dirty(w);
    }
}

static void monitor_tick(window_t *w)
{
    monitor_t *m = w->state;

    if (uptime() - m->last >= 100)
    {
        m->last = uptime();
        monitor_sample(m);
        window_dirty(w);
    }
}

static int monitor_open(window_t *w, const char *arg)
{
    (void)arg;

    monitor_t *m = calloc(1, sizeof(monitor_t));

    if (!m)
    {
        return -1;
    }

    m->selected = -1;
    monitor_sample(m);
    w->state = m;
    window_set_title(w, "Monitor", "8 cores · processes");
    return 0;
}

static void monitor_close(window_t *w)
{
    free(w->state);
}

const app_t app_monitor = {"Monitor", 700, 520, monitor_open, monitor_draw, monitor_event, monitor_tick, monitor_close};

typedef struct viewer
{
    image_t image;
    int loaded;
    char path[256];
} viewer_t;

static void viewer_draw(window_t *w, canvas_t *c)
{
    viewer_t *v = w->state;

    draw_fill(c, 0, 0, c->width, c->height, T.dark ? 0x0E1012 : 0xE4E6E9);

    if (!v->loaded)
    {
        draw_text(c, font_ui, S(13), 20, 36, "This image could not be read (PNG, JPEG and PPM work).", T.i3);
        return;
    }

    int maxw = c->width - 32;
    int maxh = c->height - 32;
    float scale = 1.0f;

    if (v->image.width > maxw || v->image.height > maxh)
    {
        float sx = (float)maxw / (float)v->image.width;
        float sy = (float)maxh / (float)v->image.height;

        scale = sx < sy ? sx : sy;
    }

    int dw = (int)((float)v->image.width * scale);
    int dh = (int)((float)v->image.height * scale);
    int ox = (c->width - dw) / 2;
    int oy = (c->height - dh) / 2;

    for (int y = 0; y < dh; y++)
    {
        int sy = (int)((float)y / scale);

        for (int x = 0; x < dw; x++)
        {
            int sx = (int)((float)x / scale);
            uint32_t p = v->image.pixels[(long)sy * v->image.width + sx];

            draw_pixel(c, ox + x, oy + y, p & 0xFFFFFF, (int)(p >> 24));
        }
    }
}

static int viewer_open(window_t *w, const char *arg)
{
    viewer_t *v = calloc(1, sizeof(viewer_t));

    if (!v)
    {
        return -1;
    }

    snprintf(v->path, sizeof(v->path), "%s", arg ? arg : "");
    v->loaded = image_load(&v->image, v->path) == 0;
    w->state = v;

    char crumb[200];

    if (v->loaded)
    {
        snprintf(crumb, sizeof(crumb), "%s · %d × %d", v->path, v->image.width, v->image.height);
    }
    else
    {
        snprintf(crumb, sizeof(crumb), "%s", v->path);
    }

    window_set_title(w, "Viewer", crumb);
    return 0;
}

static void viewer_close(window_t *w)
{
    viewer_t *v = w->state;

    if (v->loaded)
    {
        image_free(&v->image);
    }

    free(v);
}

const app_t app_viewer = {"Viewer", 820, 560, viewer_open, viewer_draw, 0, 0, viewer_close};

#define EDITOR_LINES 4096

typedef struct editor
{
    char path[256];
    char *lines[EDITOR_LINES];
    int count;
    int row;
    int column;
    int top;
    int modified;
    int readonly;
} editor_t;

static void editor_line_set(editor_t *e, int index, const char *text, int length)
{
    free(e->lines[index]);
    e->lines[index] = malloc((size_t)length + 1);

    if (e->lines[index])
    {
        memcpy(e->lines[index], text, (size_t)length);
        e->lines[index][length] = 0;
    }
}

static void editor_load(editor_t *e)
{
    FILE *f = fopen(e->path, "r");

    e->count = 0;

    if (f)
    {
        char buffer[1024];

        while (e->count < EDITOR_LINES && fgets(buffer, sizeof(buffer), f))
        {
            int length = (int)strcspn(buffer, "\r\n");
            int binary = 0;

            for (int i = 0; i < length; i++)
            {
                if ((unsigned char)buffer[i] < 9)
                {
                    binary = 1;
                }
            }

            if (binary)
            {
                e->readonly = 1;
                editor_line_set(e, 0, "This file is not text: it cannot be shown in the Editor.", 57);
                e->count = 1;
                break;
            }

            editor_line_set(e, e->count++, buffer, length);
        }

        fclose(f);
    }

    if (e->count == 0)
    {
        editor_line_set(e, 0, "", 0);
        e->count = 1;
    }
}

static int editor_save(editor_t *e)
{
    FILE *f = fopen(e->path, "w");

    if (!f)
    {
        return -1;
    }

    for (int i = 0; i < e->count; i++)
    {
        fputs(e->lines[i] ? e->lines[i] : "", f);

        if (i + 1 < e->count || (e->lines[i] && e->lines[i][0]))
        {
            fputc('\n', f);
        }
    }

    fclose(f);
    e->modified = 0;
    return 0;
}

static void editor_title(window_t *w, editor_t *e)
{
    char crumb[280];

    snprintf(crumb, sizeof(crumb), "%s%s", e->path, e->modified ? " · edited" : e->readonly ? " · read only" : "");
    window_set_title(w, "Editor", crumb);
}

static void editor_draw(window_t *w, canvas_t *c)
{
    editor_t *e = w->state;
    int line_h = SI(19);
    int cell = text_width(font_mono, S(13), "M");
    int gutter = cell * 5 + 16;
    int rows = (c->height - 16) / line_h;

    draw_fill(c, 0, 0, c->width, c->height, T.w);
    draw_fill(c, 0, 0, gutter - 8, c->height, T.w2);

    for (int k = 0; k < rows && e->top + k < e->count; k++)
    {
        int index = e->top + k;
        int y = 8 + k * line_h;
        char number[12];

        if (index == e->row)
        {
            draw_blend(c, gutter - 8, y, c->width - gutter + 8, line_h, T.a, T.dark ? 18 : 14);
        }

        snprintf(number, sizeof(number), "%d", index + 1);
        draw_text(c, font_mono, S(11.5f), gutter - 16 - text_width(font_mono, S(11.5f), number), y + line_h - 5, number,
                  index == e->row ? T.i2 : T.i3);

        const char *text = e->lines[index] ? e->lines[index] : "";
        uint32_t color = T.i;
        const char *p = text;

        while (*p == ' ')
        {
            p++;
        }

        if ((p[0] == '/' && p[1] == '/') || p[0] == '#' || (p[0] == '-' && p[1] == '-'))
        {
            color = T.i3;
        }

        draw_text(c, font_mono, S(13), gutter, y + line_h - 5, text, color);

        if (index == e->row && !e->readonly)
        {
            char prefix[1024];
            int n = e->column < (int)sizeof(prefix) - 1 ? e->column : (int)sizeof(prefix) - 1;

            memcpy(prefix, text, (size_t)n);
            prefix[n] = 0;
            draw_fill(c, gutter + text_width(font_mono, S(13), prefix), y + 2, 2, line_h - 4, T.a);
        }
    }
}

static void editor_insert(editor_t *e, const char *text)
{
    char *line = e->lines[e->row] ? e->lines[e->row] : "";
    int length = (int)strlen(line);
    int add = (int)strlen(text);
    char *next = malloc((size_t)length + (size_t)add + 1);

    if (!next)
    {
        return;
    }

    memcpy(next, line, (size_t)e->column);
    memcpy(next + e->column, text, (size_t)add);
    memcpy(next + e->column + add, line + e->column, (size_t)(length - e->column) + 1);
    free(e->lines[e->row]);
    e->lines[e->row] = next;
    e->column += add;
    e->modified = 1;
}

static void editor_event(window_t *w, const input_event_t *ev, int x, int y)
{
    editor_t *e = w->state;
    int line_h = SI(19);
    int rows = (w->content.height - 16) / line_h;
    int was_modified = e->modified;

    if (ev->type == INPUT_WHEEL)
    {
        e->top -= ev->value * 3;
        e->top = e->top < 0 ? 0 : e->top >= e->count ? e->count - 1 : e->top;
        window_dirty(w);
        return;
    }

    if (ev->type == INPUT_BUTTON && ev->value == 1)
    {
        int cell = text_width(font_mono, S(13), "M");
        int gutter = cell * 5 + 16;
        int row = e->top + (y - 8) / line_h;

        if (row >= 0 && row < e->count)
        {
            int length = e->lines[row] ? (int)strlen(e->lines[row]) : 0;

            e->row = row;
            e->column = (x - gutter + cell / 2) / cell;
            e->column = e->column < 0 ? 0 : e->column > length ? length : e->column;
            window_dirty(w);
        }

        return;
    }

    if (ev->type != INPUT_KEY || ev->value == 0 || e->readonly)
    {
        return;
    }

    int length = e->lines[e->row] ? (int)strlen(e->lines[e->row]) : 0;

    if ((ev->modifiers & INPUT_CTRL) && (ev->text | 0x20) == 's')
    {
        if (editor_save(e) == 0)
        {
            notify("Editor", "Saved", e->path);
        }
        else
        {
            notify("Editor", "Could not save", e->path);
        }

        editor_title(w, e);
        window_dirty(w);
        return;
    }

    switch (ev->code)
    {
    case KEY_UP:
        e->row = e->row > 0 ? e->row - 1 : 0;
        break;
    case KEY_DOWN:
        e->row = e->row + 1 < e->count ? e->row + 1 : e->row;
        break;
    case KEY_LEFT:
        if (e->column > 0)
        {
            e->column--;
        }
        else if (e->row > 0)
        {
            e->row--;
            e->column = e->lines[e->row] ? (int)strlen(e->lines[e->row]) : 0;
        }

        break;
    case KEY_RIGHT:
        if (e->column < length)
        {
            e->column++;
        }
        else if (e->row + 1 < e->count)
        {
            e->row++;
            e->column = 0;
        }

        break;
    case KEY_HOME:
        e->column = 0;
        break;
    case KEY_END:
        e->column = length;
        break;
    case KEY_PAGEUP:
        e->row = e->row > rows ? e->row - rows : 0;
        break;
    case KEY_PAGEDOWN:
        e->row = e->row + rows < e->count ? e->row + rows : e->count - 1;
        break;
    case KEY_ENTER:
        if (e->count < EDITOR_LINES)
        {
            char *line = e->lines[e->row] ? e->lines[e->row] : "";

            for (int i = e->count; i > e->row + 1; i--)
            {
                e->lines[i] = e->lines[i - 1];
            }

            e->lines[e->row + 1] = 0;
            editor_line_set(e, e->row + 1, line + e->column, length - e->column);
            e->lines[e->row][e->column] = 0;
            e->count++;
            e->row++;
            e->column = 0;
            e->modified = 1;
        }

        break;
    case KEY_BACKSPACE:
        if (e->column > 0)
        {
            memmove(e->lines[e->row] + e->column - 1, e->lines[e->row] + e->column, (size_t)(length - e->column) + 1);
            e->column--;
            e->modified = 1;
        }
        else if (e->row > 0)
        {
            int previous = e->lines[e->row - 1] ? (int)strlen(e->lines[e->row - 1]) : 0;
            char *joined = malloc((size_t)previous + (size_t)length + 1);

            if (joined)
            {
                memcpy(joined, e->lines[e->row - 1] ? e->lines[e->row - 1] : "", (size_t)previous);
                memcpy(joined + previous, e->lines[e->row] ? e->lines[e->row] : "", (size_t)length + 1);
                free(e->lines[e->row - 1]);
                free(e->lines[e->row]);
                e->lines[e->row - 1] = joined;

                for (int i = e->row; i < e->count - 1; i++)
                {
                    e->lines[i] = e->lines[i + 1];
                }

                e->lines[--e->count] = 0;
                e->row--;
                e->column = previous;
                e->modified = 1;
            }
        }

        break;
    case KEY_DELETE:
        if (e->column < length)
        {
            memmove(e->lines[e->row] + e->column, e->lines[e->row] + e->column + 1, (size_t)(length - e->column));
            e->modified = 1;
        }

        break;
    case KEY_TAB:
        editor_insert(e, "    ");
        break;
    default:
        if (ev->text >= 32 && ev->text < 127 && !(ev->modifiers & INPUT_CTRL))
        {
            char text[2] = {(char)ev->text, 0};

            editor_insert(e, text);
        }

        break;
    }

    length = e->lines[e->row] ? (int)strlen(e->lines[e->row]) : 0;

    if (e->column > length)
    {
        e->column = length;
    }

    if (e->row < e->top)
    {
        e->top = e->row;
    }

    if (e->row >= e->top + rows)
    {
        e->top = e->row - rows + 1;
    }

    if (was_modified != e->modified)
    {
        editor_title(w, e);
    }

    window_dirty(w);
}

static int editor_open(window_t *w, const char *arg)
{
    editor_t *e = calloc(1, sizeof(editor_t));

    if (!e)
    {
        return -1;
    }

    snprintf(e->path, sizeof(e->path), "%s", arg && arg[0] ? arg : "/home/untitled.txt");
    editor_load(e);
    w->state = e;
    editor_title(w, e);
    return 0;
}

static void editor_close(window_t *w)
{
    editor_t *e = w->state;

    for (int i = 0; i < e->count; i++)
    {
        free(e->lines[i]);
    }

    free(e);
}

const app_t app_editor = {"Editor", 720, 520, editor_open, editor_draw, editor_event, 0, editor_close};
