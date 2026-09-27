#include "desktop.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

theme_t T;
font_t *font_ui;
font_t *font_bold;
font_t *font_label;
font_t *font_mono;
int screen_w;
int screen_h;

static canvas_t screen;
static canvas_t back;
static canvas_t wall;
static window_t windows[WINDOW_MAX];
static int order[WINDOW_MAX];
static int order_count;
static int next_id = 1;
static rect_t damaged;
static int has_damage;
static int pointer_x;
static int pointer_y;
static int drag_mode;
static window_t *drag_window;
static int drag_dx;
static int drag_dy;
static uint64_t last_click;
static int last_click_x;
static int last_click_y;
static int super_down;
static int super_used;
static char note_source[32];
static char note_title[64];
static char note_detail[96];
static uint64_t note_until;
static rect_t note_rect;
static rect_t note_undo;
static long clock_minute = -1;
static uint64_t core_busy[8];
static uint64_t core_idle[8];
static int core_load[8];
static uint64_t last_sample;

enum
{
    DRAG_NONE,
    DRAG_MOVE,
    DRAG_RESIZE,
};

enum
{
    STRIP_KNOC_MARK,
    STRIP_KNOC_BAR,
    STRIP_TABS,
    STRIP_ASSIST,
};

typedef struct strip_layout
{
    rect_t all;
    rect_t mark;
    rect_t bar;
    rect_t tabs[WINDOW_MAX];
    window_t *tab_window[WINDOW_MAX];
    int tab_count;
    rect_t meter;
    rect_t clock;
    rect_t assist;
    int seg1_end;
    int seg2_end;
} strip_layout_t;

static strip_layout_t strip;

float S(float size)
{
    return size * (float)T.scale / 100.0f;
}

int SI(int value)
{
    return value * T.scale / 100;
}

void desktop_log(const char *text)
{
    printf("[DESKTOP] %s\n", text);
}

static rect_t rect_union(rect_t a, rect_t b)
{
    int x0 = a.x < b.x ? a.x : b.x;
    int y0 = a.y < b.y ? a.y : b.y;
    int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    rect_t r = {x0, y0, x1 - x0, y1 - y0};

    return r;
}

void damage(int x, int y, int w, int h)
{
    rect_t r = {x - 24, y - 24, w + 48, h + 56};

    if (r.x < 0)
    {
        r.w += r.x;
        r.x = 0;
    }

    if (r.y < 0)
    {
        r.h += r.y;
        r.y = 0;
    }

    if (r.x + r.w > screen_w)
    {
        r.w = screen_w - r.x;
    }

    if (r.y + r.h > screen_h)
    {
        r.h = screen_h - r.y;
    }

    if (r.w <= 0 || r.h <= 0)
    {
        return;
    }

    damaged = has_damage ? rect_union(damaged, r) : r;
    has_damage = 1;
}

void damage_all(void)
{
    damage(0, 0, screen_w, screen_h);
}

int point_in(int px, int py, int x, int y, int w, int h)
{
    return px >= x && py >= y && px < x + w && py < y + h;
}

static const char *const accents_config[] = {"ember", "moss", "sand", "rose", "mono"};

void theme_apply(int dark, int accent, int wallpaper, int scale)
{
    T.dark = dark;
    T.accent_index = accent;
    T.wall = wallpaper;
    T.scale = scale < 100 ? 100 : scale > 200 ? 200 : scale;

    if (dark)
    {
        T.desk = 0x121416;
        T.w = 0x1A1D20;
        T.w2 = 0x1F2226;
        T.w3 = 0x262A2E;
        T.ln = 0x2E3237;
        T.ln2 = 0x3A3F45;
        T.i = 0xECEDEE;
        T.i2 = 0xA0A6AC;
        T.i3 = 0x6C737A;
        T.need = 0xE9C46A;
        T.ok = 0x5DBE8A;
        T.bad = 0xEE6B5C;
        T.term = 0x0D0F11;
    }
    else
    {
        T.desk = 0xDEE1E5;
        T.w = 0xFBFBFC;
        T.w2 = 0xF3F4F6;
        T.w3 = 0xE9EBEE;
        T.ln = 0xD8DCE0;
        T.ln2 = 0xC3C8CE;
        T.i = 0x111418;
        T.i2 = 0x535A62;
        T.i3 = 0x858C94;
        T.need = 0x8A6200;
        T.ok = 0x2E8455;
        T.bad = 0xC2412F;
        T.term = 0x15181B;
    }

    T.term_i = 0xC9D1CC;
    T.a = accent_color(accent, dark ? THEME_GRAPHITE : THEME_PAPER);
    T.a_ink = dark ? 0x121416 : 0xFFFFFF;

    if (wall.pixels)
    {
        wallpaper_draw(&wall, wallpaper, dark ? THEME_GRAPHITE : THEME_PAPER, accent);
    }

    for (int i = 0; i < WINDOW_MAX; i++)
    {
        if (windows[i].used)
        {
            windows[i].dirty = 1;
        }
    }

    damage_all();
}

static int settings_load(void)
{
    int dark = 1;
    int accent = ACCENT_EMBER;
    int wallpaper = WALL_RIDGE;
    int scale = 100;
    FILE *f = fopen("/etc/desktop.conf", "r");

    if (f)
    {
        char line[96];

        while (fgets(line, sizeof(line), f))
        {
            char *value = strchr(line, '=');

            if (!value)
            {
                continue;
            }

            *value++ = 0;
            value[strcspn(value, "\r\n")] = 0;

            if (strcmp(line, "theme") == 0)
            {
                dark = strcmp(value, "paper") != 0;
            }
            else if (strcmp(line, "accent") == 0 && accent_find(value) >= 0)
            {
                accent = accent_find(value);
            }
            else if (strcmp(line, "wallpaper") == 0 && wallpaper_find(value) >= 0)
            {
                wallpaper = wallpaper_find(value);
            }
            else if (strcmp(line, "scale") == 0)
            {
                scale = atoi(value);
            }
        }

        fclose(f);
    }

    theme_apply(dark, accent, wallpaper, scale);
    return f != 0;
}

void settings_save(void)
{
    FILE *f = fopen("/etc/desktop.conf", "w");

    if (!f)
    {
        return;
    }

    fprintf(f, "theme=%s\naccent=%s\nwallpaper=%s\nscale=%d\n", T.dark ? "graphite" : "paper",
            accents_config[T.accent_index], wallpaper_name(T.wall), T.scale);
    fclose(f);
}

void window_dirty(window_t *w)
{
    w->dirty = 1;
    damage(w->x, w->y, w->w, w->h);
}

static void content_resize(window_t *w)
{
    int cw = w->w - 2;
    int ch = w->h - TITLE_H - 1;

    if (cw < 1 || ch < 1)
    {
        return;
    }

    if (w->content.pixels && w->content.width == cw && w->content.height == ch)
    {
        return;
    }

    canvas_free(&w->content);
    canvas_create(&w->content, cw, ch);
    w->dirty = 1;
}

static int order_index(window_t *w)
{
    for (int i = 0; i < order_count; i++)
    {
        if (&windows[order[i]] == w)
        {
            return i;
        }
    }

    return -1;
}

window_t *window_focused(void)
{
    for (int i = order_count - 1; i >= 0; i--)
    {
        if (!windows[order[i]].minimized)
        {
            return &windows[order[i]];
        }
    }

    return 0;
}

void window_focus(window_t *w)
{
    int index = order_index(w);

    if (index < 0)
    {
        return;
    }

    window_t *before = window_focused();

    for (int i = index; i < order_count - 1; i++)
    {
        order[i] = order[i + 1];
    }

    order[order_count - 1] = (int)(w - windows);
    w->minimized = 0;
    damage(w->x, w->y, w->w, w->h);

    if (before && before != w)
    {
        damage(before->x, before->y, before->w, TITLE_H);
    }

    damage(0, screen_h - STRIP_H - STRIP_GAP, screen_w, STRIP_H + STRIP_GAP);
}

window_t *window_open(const app_t *app, const char *arg)
{
    for (int i = 0; i < WINDOW_MAX; i++)
    {
        window_t *w = &windows[i];

        if (w->used)
        {
            continue;
        }

        memset(w, 0, sizeof(*w));
        w->used = 1;
        w->id = next_id++;
        w->app = app;
        w->w = SI(app->width);
        w->h = SI(app->height);

        if (w->w > screen_w - 40)
        {
            w->w = screen_w - 40;
        }

        if (w->h > screen_h - STRIP_H - 60)
        {
            w->h = screen_h - STRIP_H - 60;
        }

        int cascade = (order_count % 6) * 28;

        w->x = 48 + cascade;
        w->y = 36 + cascade;
        snprintf(w->title, sizeof(w->title), "%s", app->name);
        content_resize(w);

        if (app->open && app->open(w, arg) != 0)
        {
            canvas_free(&w->content);
            w->used = 0;
            return 0;
        }

        order[order_count++] = i;
        window_focus(w);

        char text[80];

        snprintf(text, sizeof(text), "opened %s", app->name);
        desktop_log(text);
        return w;
    }

    notify("Desktop", "Too many windows", "Close a window to open another one");
    return 0;
}

void window_close(window_t *w)
{
    int index = order_index(w);

    if (w->app->close)
    {
        w->app->close(w);
    }

    damage(w->x, w->y, w->w, w->h);
    damage(0, screen_h - STRIP_H - STRIP_GAP, screen_w, STRIP_H + STRIP_GAP);
    canvas_free(&w->content);

    char text[80];

    snprintf(text, sizeof(text), "closed %s", w->app->name);
    desktop_log(text);
    w->used = 0;

    if (index >= 0)
    {
        for (int i = index; i < order_count - 1; i++)
        {
            order[i] = order[i + 1];
        }

        order_count--;
    }

    window_t *top = window_focused();

    if (top)
    {
        damage(top->x, top->y, top->w, TITLE_H);
    }
}

void window_set_title(window_t *w, const char *title, const char *crumb)
{
    if (title)
    {
        snprintf(w->title, sizeof(w->title), "%s", title);
    }

    snprintf(w->crumb, sizeof(w->crumb), "%s", crumb ? crumb : "");
    damage(w->x, w->y, w->w, TITLE_H);
}

int window_count_app(const app_t *app)
{
    int count = 0;

    for (int i = 0; i < WINDOW_MAX; i++)
    {
        count += windows[i].used && windows[i].app == app;
    }

    return count;
}

window_t *window_find_app(const app_t *app)
{
    for (int i = order_count - 1; i >= 0; i--)
    {
        if (windows[order[i]].app == app)
        {
            return &windows[order[i]];
        }
    }

    return 0;
}

static void window_place(window_t *w, int x, int y, int width, int height)
{
    damage(w->x, w->y, w->w, w->h);
    w->x = x;
    w->y = y;
    w->w = width < 240 ? 240 : width;
    w->h = height < 140 ? 140 : height;
    content_resize(w);
    damage(w->x, w->y, w->w, w->h);
}

static int work_width(void)
{
    return screen_w - assist_width();
}

static int work_height(void)
{
    return screen_h - STRIP_H - STRIP_GAP * 2;
}

static void window_maximize(window_t *w)
{
    if (w->maximized)
    {
        w->maximized = 0;
        window_place(w, w->restore.x, w->restore.y, w->restore.w, w->restore.h);
        return;
    }

    w->restore.x = w->x;
    w->restore.y = w->y;
    w->restore.w = w->w;
    w->restore.h = w->h;
    w->maximized = 1;
    window_place(w, 0, 0, work_width(), work_height());
}

static void window_snap(window_t *w, int where)
{
    if (!w->maximized)
    {
        w->restore.x = w->x;
        w->restore.y = w->y;
        w->restore.w = w->w;
        w->restore.h = w->h;
    }

    w->maximized = where == 2;

    int half = work_width() / 2;

    if (where == 0)
    {
        window_place(w, 0, 0, half, work_height());
    }
    else if (where == 1)
    {
        window_place(w, half, 0, work_width() - half, work_height());
    }
    else
    {
        window_place(w, 0, 0, work_width(), work_height());
    }
}

void notify(const char *source, const char *title, const char *detail)
{
    snprintf(note_source, sizeof(note_source), "%s", source);
    snprintf(note_title, sizeof(note_title), "%s", title);
    snprintf(note_detail, sizeof(note_detail), "%s", detail ? detail : "");
    note_until = uptime() + 500;
    damage(note_rect.x, note_rect.y, note_rect.w, note_rect.h);
    damage(screen_w - assist_width() - 360, 0, 360, 120);

    char text[200];

    snprintf(text, sizeof(text), "notification: %s: %s", title, note_detail);
    desktop_log(text);
}

static void shadow(canvas_t *c, int x, int y, int w, int h)
{
    static const int alpha[] = {46, 30, 20, 13, 8, 5, 3};

    for (int k = 0; k < 7; k++)
    {
        int spread = k + 1;

        draw_frame(c, x - spread, y - spread + 5, w + spread * 2, h + spread * 2, RADIUS + spread, 0x000000,
                   alpha[k]);
    }
}

static void blit_content(canvas_t *view, const canvas_t *content, int x, int y, int bottom)
{
    for (int j = 0; j < content->height; j++)
    {
        int ty = y + j;

        if (ty < 0 || ty >= view->height)
        {
            continue;
        }

        int corner = bottom - ty;
        int skip = 0;

        if (corner < RADIUS)
        {
            float dy = (float)(RADIUS - corner) - 0.5f;
            float dx = (float)RADIUS - sqrtf((float)(RADIUS * RADIUS) - dy * dy);

            skip = (int)(dx + 0.5f);
        }

        uint32_t *row = view->pixels + (long)ty * view->stride;
        const uint32_t *src = content->pixels + (long)j * content->stride;

        for (int i = skip; i < content->width - skip; i++)
        {
            int tx = x + i;

            if (tx >= 0 && tx < view->width)
            {
                row[tx] = src[i];
            }
        }
    }
}

static void draw_window(canvas_t *view, window_t *w, int ox, int oy, int focused)
{
    int x = w->x - ox;
    int y = w->y - oy;

    shadow(view, x, y, w->w, w->h);
    draw_round(view, x, y, w->w, w->h, RADIUS, T.w, 255);
    draw_round(view, x + 1, y + 1, w->w - 2, TITLE_H - 1, RADIUS - 1, focused ? T.w2 : T.w, 255);
    draw_fill(view, x + 1, y + TITLE_H / 2, w->w - 2, TITLE_H / 2, focused ? T.w2 : T.w);
    draw_fill(view, x + 1, y + TITLE_H - 1, w->w - 2, 1, T.ln);

    int tx = x + 12;
    int baseline = y + 22;

    tx += draw_text(view, focused ? font_bold : font_ui, S(12.5f), tx, baseline, w->title, focused ? T.i : T.i2) + 10;

    char crumb[160];

    text_fit(crumb, sizeof(crumb), font_mono, S(11.5f), w->crumb, w->w - (tx - x) - 110);
    draw_text(view, font_mono, S(11.5f), tx, baseline, crumb, T.i3);

    int bx = x + w->w - 6 - 30 * 3;

    for (int k = 0; k < 3; k++)
    {
        int kind = k == 0 ? ICON_MIN : k == 1 ? ICON_MAX : ICON_CLOSE;
        int hover = point_in(pointer_x - ox, pointer_y - oy, bx + k * 30, y + 4, 30, 26);

        if (hover)
        {
            draw_round(view, bx + k * 30, y + 4, 30, 26, 4, k == 2 ? T.bad : T.w3, 255);
        }

        draw_icon(view, kind, bx + k * 30 + 9, y + 11, 12, hover && k == 2 ? 0xFFFFFF : T.i3);
    }

    if (w->dirty && w->app->draw && w->content.pixels)
    {
        w->app->draw(w, &w->content);
        w->dirty = 0;
    }

    if (w->content.pixels)
    {
        blit_content(view, &w->content, x + 1, y + TITLE_H, y + w->h - 1);
    }

    draw_frame(view, x, y, w->w, w->h, RADIUS, T.dark ? 0x000000 : 0x8A96A3, T.dark ? 150 : 60);
    draw_frame(view, x + 1, y + 1, w->w - 2, w->h - 2, RADIUS - 1, 0xFFFFFF, T.dark ? 8 : 0);
}

static void clock_text(char *time_text, int time_size, char *date_text, int date_size)
{
    time_t now = time(0);

    if (now <= 0)
    {
        unsigned long seconds = uptime() / 100;

        snprintf(time_text, (size_t)time_size, "%02lu:%02lu", (seconds / 3600) % 24, (seconds / 60) % 60);
        snprintf(date_text, (size_t)date_size, "UPTIME");
        return;
    }

    struct tm *t = gmtime(&now);
    static const char *const days[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    static const char *const months[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                         "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

    snprintf(time_text, (size_t)time_size, "%02d:%02d", t->tm_hour, t->tm_min);
    snprintf(date_text, (size_t)date_size, "%s %d %s", days[t->tm_wday % 7], t->tm_mday, months[t->tm_mon % 12]);
}

static void strip_compute(void)
{
    int h = STRIP_H;
    int pad = 6;
    int x = 0;

    strip.mark = (rect_t){x + pad, 6, 34, 34};
    strip.bar = (rect_t){strip.mark.x + 36, 7, SI(260), 32};
    x = strip.bar.x + strip.bar.w + pad;
    strip.seg1_end = x;
    x += pad;
    strip.tab_count = 0;

    for (int i = 0; i < WINDOW_MAX; i++)
    {
        window_t *w = &windows[i];

        if (!w->used || strip.tab_count >= 9)
        {
            continue;
        }

        int width = text_width(font_ui, S(13), w->title) + 36;

        if (width > 170)
        {
            width = 170;
        }

        strip.tabs[strip.tab_count] = (rect_t){x, 6, width, 34};
        strip.tab_window[strip.tab_count++] = w;
        x += width + 2;
    }

    if (strip.tab_count == 0)
    {
        x += 4;
    }

    strip.seg2_end = x + pad - 2;
    x = strip.seg2_end + pad;
    strip.meter = (rect_t){x + 4, 14, 8 * 6 + 3, 18};
    x = strip.meter.x + strip.meter.w + 14;
    strip.clock = (rect_t){x, 6, SI(92), 34};
    x += strip.clock.w + 6;
    strip.assist = (rect_t){x, 6, SI(76), 34};
    x += strip.assist.w + pad;

    int width = x;
    int left = (work_width() - width) / 2;

    strip.all = (rect_t){left, screen_h - STRIP_GAP - h, width, h};

    rect_t *items[] = {&strip.mark, &strip.bar, &strip.meter, &strip.clock, &strip.assist};

    for (unsigned k = 0; k < sizeof(items) / sizeof(items[0]); k++)
    {
        items[k]->x += left;
        items[k]->y += strip.all.y;
    }

    for (int i = 0; i < strip.tab_count; i++)
    {
        strip.tabs[i].x += left;
        strip.tabs[i].y += strip.all.y;
    }

    strip.seg1_end += left;
    strip.seg2_end += left;
}

void presence_draw(canvas_t *c, int x, int y, int active)
{
    uint64_t t = uptime();

    for (int k = 0; k < 4; k++)
    {
        int alpha = 255;

        if (active)
        {
            int phase = (int)((t / 4 + (uint64_t)k * 5) % 16);

            alpha = 90 + (phase < 8 ? phase : 16 - phase) * 20;
        }

        draw_round(c, x + (k % 2) * 8, y + (k / 2) * 8, 5, 5, 1, T.a, alpha);
    }
}

static void draw_strip(canvas_t *view, int ox, int oy)
{
    rect_t a = strip.all;
    int x = a.x - ox;
    int y = a.y - oy;

    shadow(view, x, y, a.w, a.h);
    draw_round(view, x, y, a.w, a.h, 8, T.w2, 255);
    draw_frame(view, x, y, a.w, a.h, 8, T.dark ? 0x000000 : 0x8A96A3, T.dark ? 150 : 60);
    draw_fill(view, strip.seg1_end - ox, y + 8, 1, a.h - 16, T.ln);
    draw_fill(view, strip.seg2_end - ox, y + 8, 1, a.h - 16, T.ln);

    rect_t m = strip.mark;

    if (point_in(pointer_x, pointer_y, m.x, m.y, m.w, m.h))
    {
        draw_round(view, m.x - ox, m.y - oy, m.w, m.h, 5, T.w3, 255);
    }

    draw_line(view, (float)(m.x - ox + 13), (float)(m.y - oy + 8), (float)(m.x - ox + 13), (float)(m.y - oy + 26), T.i, 255);
    draw_line(view, (float)(m.x - ox + 13), (float)(m.y - oy + 18), (float)(m.x - ox + 23), (float)(m.y - oy + 9), T.i, 255);
    draw_line(view, (float)(m.x - ox + 16), (float)(m.y - oy + 15), (float)(m.x - ox + 23), (float)(m.y - oy + 26), T.i, 255);

    rect_t b = strip.bar;

    draw_round(view, b.x - ox, b.y - oy, b.w, b.h, 5, T.w, 255);
    draw_frame(view, b.x - ox, b.y - oy, b.w, b.h, 5, T.ln, 255);
    draw_icon(view, ICON_SEARCH, b.x - ox + 10, b.y - oy + 9, 13, T.i3);
    draw_text(view, font_ui, S(13), b.x - ox + 31, b.y - oy + 21, "Open, find or ask", T.i3);

    int kw = text_width(font_label, S(9.5f), "SUPER") + 10;

    draw_frame(view, b.x - ox + b.w - kw - 8, b.y - oy + 8, kw, 16, 3, T.ln2, 255);
    draw_text(view, font_label, S(9.5f), b.x - ox + b.w - kw - 3, b.y - oy + 20, "SUPER", T.i3);

    window_t *focused = window_focused();

    for (int i = 0; i < strip.tab_count; i++)
    {
        rect_t t = strip.tabs[i];
        window_t *w = strip.tab_window[i];
        int on = w == focused;
        char index[4];
        char title[48];

        if (on)
        {
            draw_round(view, t.x - ox, t.y - oy, t.w, t.h, 5, T.w3, 255);
        }
        else if (point_in(pointer_x, pointer_y, t.x, t.y, t.w, t.h))
        {
            draw_round(view, t.x - ox, t.y - oy, t.w, t.h, 5, T.w3, 140);
        }

        snprintf(index, sizeof(index), "%d", i + 1);
        draw_text(view, font_label, S(9.5f), t.x - ox + 11, t.y - oy + 21, index, T.i3);
        text_fit(title, sizeof(title), font_ui, S(13), w->title, t.w - 34);
        draw_text(view, on ? font_bold : font_ui, S(13), t.x - ox + 24, t.y - oy + 22, title, on ? T.i : T.i2);
        draw_circle(view, (float)(t.x - ox + t.w / 2), (float)(t.y - oy + t.h - 3), 2.0f,
                    on ? T.a : T.i3, w->minimized ? 90 : 255);
    }

    rect_t me = strip.meter;

    for (int k = 0; k < 8; k++)
    {
        int bar = 4 + core_load[k] * 14 / 100;
        int bx = me.x - ox + k * 6 + (k >= 4 ? 3 : 0);

        draw_round(view, bx, me.y - oy + me.h - bar, 4, bar, 1, k >= 4 ? T.a : T.i3, k >= 4 ? 255 : 180);
    }

    char time_text[16];
    char date_text[24];

    clock_text(time_text, sizeof(time_text), date_text, sizeof(date_text));

    rect_t c = strip.clock;
    int tw = text_width(font_mono, S(12), time_text);
    int dw = text_width(font_label, S(9), date_text);

    draw_icon(view, ICON_NETWORK, c.x - ox - 2, c.y - oy + 10, 14, T.i2);
    draw_text(view, font_mono, S(12), c.x - ox + c.w - tw - 4, c.y - oy + 16, time_text, T.i);
    draw_text(view, font_label, S(9), c.x - ox + c.w - dw - 4, c.y - oy + 29, date_text, T.i3);

    rect_t as = strip.assist;
    int on = assist_open();

    if (on || point_in(pointer_x, pointer_y, as.x, as.y, as.w, as.h))
    {
        draw_round(view, as.x - ox, as.y - oy, as.w, as.h, 5, on ? T.w3 : T.w3, on ? 255 : 140);
    }

    presence_draw(view, as.x - ox + 11, as.y - oy + 10, on);
    draw_text(view, font_bold, S(13), as.x - ox + 31, as.y - oy + 22, "Knoc", T.i);
}

static void draw_note(canvas_t *view, int ox, int oy)
{
    if (uptime() > note_until || !note_title[0])
    {
        note_rect.w = 0;
        return;
    }

    int w = SI(300);
    int h = note_detail[0] ? 74 : 56;
    int x = work_width() - w - 16;
    int y = 16;

    note_rect = (rect_t){x, y, w, h};
    note_undo = (rect_t){0, 0, 0, 0};
    shadow(view, x - ox, y - oy, w, h);
    draw_round(view, x - ox, y - oy, w, h, 6, T.w2, 255);
    draw_frame(view, x - ox, y - oy, w, h, 6, T.dark ? 0x000000 : 0x8A96A3, T.dark ? 150 : 60);

    char upper[32];
    int n = 0;

    for (; note_source[n] && n < 31; n++)
    {
        upper[n] = (char)(note_source[n] >= 'a' && note_source[n] <= 'z' ? note_source[n] - 32 : note_source[n]);
    }

    upper[n] = 0;
    draw_text(view, font_label, S(9.5f), x - ox + 12, y - oy + 19, upper, T.i3);
    draw_text(view, font_label, S(9.5f), x - ox + w - 40, y - oy + 19, "NOW", T.i3);
    draw_text(view, font_bold, S(13), x - ox + 12, y - oy + 39, note_title, T.i);

    if (note_detail[0])
    {
        char detail[96];

        text_fit(detail, sizeof(detail), font_ui, S(12), note_detail, w - 24);
        draw_text(view, font_ui, S(12), x - ox + 12, y - oy + 59, detail, T.i2);
    }
}

static void compose(void)
{
    if (!has_damage)
    {
        return;
    }

    rect_t r = damaged;

    has_damage = 0;

    canvas_t view = {back.pixels + (long)r.y * back.stride + r.x, r.w, r.h, back.stride};

    for (int j = 0; j < r.h; j++)
    {
        memcpy(view.pixels + (long)j * view.stride, wall.pixels + (long)(r.y + j) * wall.stride + r.x,
               (size_t)r.w * 4);
    }

    draw_text(&view, font_label, 10, 36 - r.x, screen_h - 84 - r.y, "KNOCOS 0.38", T.dark ? 0x4A5058 : 0x9AA1A8);

    window_t *focused = window_focused();

    for (int k = 0; k < order_count; k++)
    {
        window_t *w = &windows[order[k]];

        if (w->minimized)
        {
            continue;
        }

        rect_t wr = {w->x - 24, w->y - 24, w->w + 48, w->h + 56};

        if (wr.x < r.x + r.w && wr.x + wr.w > r.x && wr.y < r.y + r.h && wr.y + wr.h > r.y)
        {
            draw_window(&view, w, r.x, r.y, w == focused);
        }
    }

    draw_note(&view, r.x, r.y);
    strip_compute();
    draw_strip(&view, r.x, r.y);
    assist_draw(&view, r.x, r.y);
    knoc_bar_draw(&view, r.x, r.y);

    for (int j = 0; j < r.h; j++)
    {
        memcpy(screen.pixels + (long)(r.y + j) * screen.stride + r.x, back.pixels + (long)(r.y + j) * back.stride + r.x,
               (size_t)r.w * 4);
    }

    screen_update(&screen, r.x, r.y, r.w, r.h);
}

static window_t *window_at(int x, int y)
{
    for (int k = order_count - 1; k >= 0; k--)
    {
        window_t *w = &windows[order[k]];

        if (!w->minimized && point_in(x, y, w->x, w->y, w->w, w->h))
        {
            return w;
        }
    }

    return 0;
}

static void cycle_windows(void)
{
    if (order_count < 2)
    {
        return;
    }

    window_focus(&windows[order[0]]);
}

void open_path(const char *path)
{
    const char *dot = strrchr(path, '.');

    if (dot && (strcmp(dot, ".png") == 0 || strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0 ||
                strcmp(dot, ".ppm") == 0))
    {
        window_open(&app_viewer, path);
    }
    else if (strncmp(path, "/bin/", 5) == 0 || strncmp(path, "/usr/bin/", 9) == 0)
    {
        window_open(&app_terminal, path);
    }
    else
    {
        window_open(&app_editor, path);
    }
}

static void screenshot(void)
{
    char path[80];

    mkdir("/home/Pictures");
    mkdir("/home/Pictures/Screenshots");

    for (int n = 1; n < 1000; n++)
    {
        file_stat_t st;

        snprintf(path, sizeof(path), "/home/Pictures/Screenshots/screenshot-%d.ppm", n);

        if (stat(path, &st) != 0)
        {
            break;
        }
    }

    FILE *f = fopen(path, "w");

    if (!f)
    {
        notify("Screenshot", "Could not save the screenshot", path);
        return;
    }

    fprintf(f, "P6\n%d %d\n255\n", screen_w, screen_h);

    unsigned char *row = malloc((size_t)screen_w * 3);

    for (int y = 0; row && y < screen_h; y++)
    {
        for (int x = 0; x < screen_w; x++)
        {
            uint32_t p = back.pixels[(long)y * back.stride + x];

            row[x * 3] = (unsigned char)(p >> 16);
            row[x * 3 + 1] = (unsigned char)(p >> 8);
            row[x * 3 + 2] = (unsigned char)p;
        }

        fwrite(row, 1, (size_t)screen_w * 3, f);
    }

    free(row);
    fclose(f);
    notify("Screenshot", "Saved a screenshot", path);
}

static int handle_shortcut(const input_event_t *e)
{
    if (e->code == KEY_SUPER)
    {
        if (e->value == 1)
        {
            super_down = 1;
            super_used = 0;
        }
        else if (e->value == 0)
        {
            if (super_down && !super_used)
            {
                knoc_bar_toggle();
            }

            super_down = 0;
        }

        return 1;
    }

    if (e->value == 0)
    {
        return 0;
    }

    if (e->code == KEY_PRINT)
    {
        screenshot();
        return 1;
    }

    if ((e->modifiers & INPUT_ALT) && e->code == KEY_TAB)
    {
        cycle_windows();
        return 1;
    }

    if ((e->modifiers & INPUT_CTRL) && e->code == KEY_SPACE)
    {
        assist_toggle();
        return 1;
    }

    if (!(e->modifiers & INPUT_SUPER))
    {
        return 0;
    }

    super_used = 1;

    window_t *w = window_focused();
    uint32_t c = e->text | 0x20;

    if (c >= '1' && c <= '9')
    {
        int index = (int)(c - '1');

        strip_compute();

        if (index < strip.tab_count)
        {
            window_focus(strip.tab_window[index]);
        }
    }
    else if (c == 't')
    {
        window_open(&app_terminal, 0);
    }
    else if (c == 'e')
    {
        window_open(&app_files, "/home");
    }
    else if (c == 'a')
    {
        assist_toggle();
    }
    else if (c == 'q' && w)
    {
        window_close(w);
    }
    else if (e->code == KEY_LEFT && w)
    {
        window_snap(w, 0);
    }
    else if (e->code == KEY_RIGHT && w)
    {
        window_snap(w, 1);
    }
    else if (e->code == KEY_UP && w)
    {
        window_snap(w, 2);
    }
    else if (e->code == KEY_DOWN && w)
    {
        if (w->maximized)
        {
            window_maximize(w);
        }
        else
        {
            w->minimized = 1;
            damage(w->x, w->y, w->w, w->h);
            damage(strip.all.x, strip.all.y, strip.all.w, strip.all.h);
        }
    }

    return 1;
}

static void press(int x, int y, int button)
{
    strip_compute();

    if (point_in(x, y, strip.all.x, strip.all.y, strip.all.w, strip.all.h))
    {
        if (point_in(x, y, strip.mark.x, strip.mark.y, strip.mark.w, strip.mark.h) ||
            point_in(x, y, strip.bar.x, strip.bar.y, strip.bar.w, strip.bar.h))
        {
            knoc_bar_toggle();
        }
        else if (point_in(x, y, strip.assist.x, strip.assist.y, strip.assist.w, strip.assist.h))
        {
            assist_toggle();
        }
        else
        {
            for (int i = 0; i < strip.tab_count; i++)
            {
                rect_t t = strip.tabs[i];

                if (point_in(x, y, t.x, t.y, t.w, t.h))
                {
                    window_t *w = strip.tab_window[i];

                    if (w == window_focused() && !w->minimized)
                    {
                        w->minimized = 1;
                        damage(w->x, w->y, w->w, w->h);
                        damage(strip.all.x, strip.all.y, strip.all.w, strip.all.h);
                    }
                    else
                    {
                        window_focus(w);
                    }
                }
            }
        }

        return;
    }

    if (note_rect.w && point_in(x, y, note_rect.x, note_rect.y, note_rect.w, note_rect.h))
    {
        note_until = 0;
        damage(note_rect.x, note_rect.y, note_rect.w, note_rect.h);
        return;
    }

    window_t *w = window_at(x, y);

    if (!w)
    {
        return;
    }

    if (w != window_focused())
    {
        window_focus(w);
    }

    int lx = x - w->x;
    int ly = y - w->y;

    if (ly < TITLE_H)
    {
        int bx = w->w - 6 - 30 * 3;

        if (lx >= bx && lx < bx + 90 && ly >= 4 && ly < 30)
        {
            int k = (lx - bx) / 30;

            if (k == 0)
            {
                w->minimized = 1;
                damage(w->x, w->y, w->w, w->h);
                damage(strip.all.x, strip.all.y, strip.all.w, strip.all.h);
            }
            else if (k == 1)
            {
                window_maximize(w);
            }
            else
            {
                window_close(w);
            }

            return;
        }

        uint64_t now = uptime();

        if (now - last_click < 40 && abs(x - last_click_x) < 6 && abs(y - last_click_y) < 6)
        {
            window_maximize(w);
            last_click = 0;
            return;
        }

        last_click = now;
        last_click_x = x;
        last_click_y = y;
        drag_mode = DRAG_MOVE;
        drag_window = w;
        drag_dx = lx;
        drag_dy = ly;
        return;
    }

    if (lx >= w->w - 14 && ly >= w->h - 14)
    {
        drag_mode = DRAG_RESIZE;
        drag_window = w;
        drag_dx = w->w - lx;
        drag_dy = w->h - ly;
        return;
    }

    if (w->app->event)
    {
        input_event_t local = {INPUT_BUTTON, (uint16_t)button, 1, x, y, 0, 0};

        w->app->event(w, &local, lx - 1, ly - TITLE_H);
    }
}

static void pointer_event(const input_event_t *e)
{
    int old_x = pointer_x;
    int old_y = pointer_y;

    pointer_x = e->x;
    pointer_y = e->y;

    if (e->type == INPUT_MOVE)
    {
        if (drag_mode == DRAG_MOVE && drag_window)
        {
            window_t *w = drag_window;

            if (w->maximized)
            {
                w->maximized = 0;
                drag_dx = w->restore.w / 2;
                window_place(w, pointer_x - drag_dx, pointer_y - drag_dy, w->restore.w, w->restore.h);
            }

            int ny = pointer_y - drag_dy;

            damage(w->x, w->y, w->w, w->h);
            w->x = pointer_x - drag_dx;
            w->y = ny < 0 ? 0 : ny;
            damage(w->x, w->y, w->w, w->h);
            return;
        }

        if (drag_mode == DRAG_RESIZE && drag_window)
        {
            window_t *w = drag_window;

            window_place(w, w->x, w->y, pointer_x - w->x + drag_dx, pointer_y - w->y + drag_dy);
            return;
        }

        int in_strip = point_in(pointer_x, pointer_y, strip.all.x, strip.all.y, strip.all.w, strip.all.h) ||
                       point_in(old_x, old_y, strip.all.x, strip.all.y, strip.all.w, strip.all.h);

        if (in_strip)
        {
            damage(strip.all.x, strip.all.y, strip.all.w, strip.all.h);
        }

        window_t *top = window_focused();

        if (top && (point_in(pointer_x, pointer_y, top->x, top->y, top->w, TITLE_H) ||
                    point_in(old_x, old_y, top->x, top->y, top->w, TITLE_H)))
        {
            damage(top->x + top->w - 100, top->y, 100, TITLE_H);
        }

        if (top && top->app->event && point_in(pointer_x, pointer_y, top->x, top->y + TITLE_H, top->w, top->h - TITLE_H))
        {
            top->app->event(top, e, pointer_x - top->x - 1, pointer_y - top->y - TITLE_H);
        }

        return;
    }

    if (e->type == INPUT_BUTTON)
    {
        if (e->value)
        {
            if (assist_event(e) || knoc_bar_event(e))
            {
                return;
            }

            press(pointer_x, pointer_y, e->code);
        }
        else
        {
            if (drag_mode == DRAG_MOVE && drag_window)
            {
                if (pointer_x <= 2)
                {
                    window_snap(drag_window, 0);
                }
                else if (pointer_x >= work_width() - 3)
                {
                    window_snap(drag_window, 1);
                }
                else if (pointer_y <= 2)
                {
                    window_snap(drag_window, 2);
                }
            }

            drag_mode = DRAG_NONE;
            drag_window = 0;

            window_t *top = window_focused();

            if (top && top->app->event && point_in(pointer_x, pointer_y, top->x, top->y + TITLE_H, top->w, top->h - TITLE_H))
            {
                top->app->event(top, e, pointer_x - top->x - 1, pointer_y - top->y - TITLE_H);
            }
        }

        return;
    }

    if (e->type == INPUT_WHEEL)
    {
        if (assist_event(e) || knoc_bar_event(e))
        {
            return;
        }

        window_t *w = window_at(pointer_x, pointer_y);

        if (w && w->app->event)
        {
            w->app->event(w, e, pointer_x - w->x - 1, pointer_y - w->y - TITLE_H);
        }
    }
}

static void key_event(const input_event_t *e)
{
    if (e->code != KEY_SUPER && super_down)
    {
        super_used = 1;
    }

    if (knoc_bar_open() && knoc_bar_event(e))
    {
        return;
    }

    if (handle_shortcut(e))
    {
        return;
    }

    if (assist_open() && assist_event(e))
    {
        return;
    }

    window_t *w = window_focused();

    if (w && w->app->event)
    {
        w->app->event(w, e, -1, -1);
    }
}

static void sample_cores(void)
{
    uint64_t now = uptime();

    if (now - last_sample < 100)
    {
        return;
    }

    last_sample = now;

    for (unsigned k = 0; k < 8; k++)
    {
        cpu_info_t info;

        if (cpuinfo(k, &info) != 0)
        {
            core_load[k] = 0;
            continue;
        }

        uint64_t busy = info.busy_ticks - core_busy[k];
        uint64_t idle = info.idle_ticks - core_idle[k];

        core_busy[k] = info.busy_ticks;
        core_idle[k] = info.idle_ticks;
        core_load[k] = busy + idle ? (int)(busy * 100 / (busy + idle)) : 0;
    }

    clock_minute = (long)(now / 6000);
    damage(strip.all.x, strip.all.y, strip.all.w, strip.all.h);
}

static void make_cursor(void)
{
    static uint32_t pixels[64 * 64];
    static const float shape[][2] = {{1, 1}, {1, 17}, {5, 13}, {8, 20}, {10.5f, 19}, {7.5f, 12}, {13, 12}};
    int n = sizeof(shape) / sizeof(shape[0]);

    for (int y = 0; y < 64; y++)
    {
        for (int x = 0; x < 64; x++)
        {
            int inside = 0;
            int near_edge = 0;

            for (int sy = 0; sy < 4; sy++)
            {
                for (int sx = 0; sx < 4; sx++)
                {
                    float px = (float)x + (sx + 0.5f) / 4.0f;
                    float py = (float)y + (sy + 0.5f) / 4.0f;
                    int c = 0;
                    float best = 99;

                    for (int i = 0, j = n - 1; i < n; j = i++)
                    {
                        float xi = shape[i][0], yi = shape[i][1], xj = shape[j][0], yj = shape[j][1];

                        if (((yi > py) != (yj > py)) && (px < (xj - xi) * (py - yi) / (yj - yi) + xi))
                        {
                            c = !c;
                        }

                        float dx = xj - xi, dy = yj - yi;
                        float t = ((px - xi) * dx + (py - yi) * dy) / (dx * dx + dy * dy);

                        t = t < 0 ? 0 : t > 1 ? 1 : t;

                        float ex = xi + t * dx - px, ey = yi + t * dy - py;
                        float d = sqrtf(ex * ex + ey * ey);

                        best = d < best ? d : best;
                    }

                    if (c)
                    {
                        inside++;
                    }

                    if (best < 1.1f)
                    {
                        near_edge++;
                    }
                }
            }

            uint32_t alpha = (uint32_t)((inside + (near_edge - inside > 0 ? near_edge - inside : 0)) * 255 / 16);
            uint32_t color = near_edge > inside / 2 && near_edge > 4 ? 0x111418 : 0xFFFFFF;

            if (alpha > 255)
            {
                alpha = 255;
            }

            pixels[y * 64 + x] = alpha ? (alpha << 24) | color : 0;
        }
    }

    screen_cursor(pixels, 1, 1);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (screen_open(&screen) != 0)
    {
        printf("desktop: no screen (start QEMU with a virtio-gpu device: make run-gui)\n");
        return 1;
    }

    screen_w = screen.width;
    screen_h = screen.height;
    font_ui = font_load("/fonts/HankenGrotesk-Regular.ttf");
    font_bold = font_load("/fonts/HankenGrotesk-SemiBold.ttf");
    font_label = font_load("/fonts/MartianMono-Regular.ttf");
    font_mono = font_load("/fonts/JetBrainsMono-Regular.ttf");

    if (!font_ui || !font_bold || !font_label || !font_mono)
    {
        printf("desktop: the fonts in /fonts are missing\n");
        return 1;
    }

    if (canvas_create(&back, screen_w, screen_h) != 0 || canvas_create(&wall, screen_w, screen_h) != 0)
    {
        printf("desktop: not enough memory\n");
        return 1;
    }

    int configured = settings_load();

    knoc_init();
    make_cursor();
    pointer_x = screen_w / 2;
    pointer_y = screen_h / 2;
    if (!configured)
    {
        window_open(&app_settings, 0);
        notify("Welcome", "Welcome to KnocOS", "Pick a look here. Press Super to open, find or ask.");
        settings_save();
        desktop_log("first start: welcome");
    }

    damage_all();
    compose();
    printf("[DESKTOP] ready: %dx%d, %s theme, %s accent, %s wallpaper\n", screen_w, screen_h,
           T.dark ? "graphite" : "paper", accents_config[T.accent_index], wallpaper_name(T.wall));

    input_event_t events[32];

    while (1)
    {
        int count = input_read(events, 32, has_damage ? 0 : 2);

        for (int i = 0; i < count; i++)
        {
            if (events[i].type == INPUT_KEY)
            {
                key_event(&events[i]);
            }
            else
            {
                pointer_event(&events[i]);
            }
        }

        for (int i = 0; i < WINDOW_MAX; i++)
        {
            if (windows[i].used && windows[i].app->tick)
            {
                windows[i].app->tick(&windows[i]);
            }
        }

        assist_tick();
        sample_cores();

        if (note_title[0] && note_until && uptime() > note_until)
        {
            note_until = 0;
            damage(note_rect.x, note_rect.y, note_rect.w, note_rect.h);
        }

        compose();
    }
}
