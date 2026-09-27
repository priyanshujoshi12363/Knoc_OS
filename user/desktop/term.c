#include "desktop.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TERM_COLUMNS_MAX 200
#define TERM_ROWS_MAX 80
#define PARAMS_MAX 8

typedef struct cell
{
    uint32_t code;
    uint8_t fg;
    uint8_t bg;
    uint8_t flags;
    uint8_t unused;
} cell_t;

typedef struct term
{
    int pty;
    int pid;
    int ended;
    int columns;
    int rows;
    int cell_w;
    int cell_h;
    int cx;
    int cy;
    int saved_x;
    int saved_y;
    int top;
    int bottom;
    int wrap;
    int cursor_on;
    uint8_t fg;
    uint8_t bg;
    uint8_t flags;
    int state;
    int params[PARAMS_MAX];
    int param_count;
    int private_mode;
    uint32_t utf8;
    int utf8_left;
    int full;
    int drawn_cy;
    uint32_t *seen_pixels;
    uint64_t last_check;
    cell_t grid[TERM_ROWS_MAX][TERM_COLUMNS_MAX];
    uint8_t dirty[TERM_ROWS_MAX];
} term_t;

enum
{
    FG = 16,
    BG = 17,
};

static const uint32_t ansi[16] = {
    0x2A2E33, 0xEE6B5C, 0x5DBE8A, 0xE9C46A, 0x7F9CC8, 0xC792B9, 0x6FB8B0, 0xC9CDD0,
    0x6C737A, 0xFF8A7C, 0x7FD6A4, 0xF4D68A, 0xA3BCE0, 0xDDB0D2, 0x8FD4CC, 0xECEDEE,
};

static uint32_t color_of(uint8_t index)
{
    if (index == FG)
    {
        return T.term_i;
    }

    if (index == BG)
    {
        return T.term;
    }

    return ansi[index & 15];
}

static void clear_cells(term_t *t, int row, int from, int to)
{
    for (int x = from; x < to && x < t->columns; x++)
    {
        t->grid[row][x].code = ' ';
        t->grid[row][x].fg = t->fg;
        t->grid[row][x].bg = t->bg;
        t->grid[row][x].flags = 0;
    }

    t->dirty[row] = 1;
}

static void scroll_up(term_t *t, int top, int bottom, int count)
{
    for (int n = 0; n < count; n++)
    {
        for (int y = top; y < bottom; y++)
        {
            memcpy(t->grid[y], t->grid[y + 1], sizeof(t->grid[y]));
            t->dirty[y] = 1;
        }

        clear_cells(t, bottom, 0, t->columns);
    }
}

static void scroll_down(term_t *t, int top, int bottom, int count)
{
    for (int n = 0; n < count; n++)
    {
        for (int y = bottom; y > top; y--)
        {
            memcpy(t->grid[y], t->grid[y - 1], sizeof(t->grid[y]));
            t->dirty[y] = 1;
        }

        clear_cells(t, top, 0, t->columns);
    }
}

static void line_feed(term_t *t)
{
    if (t->cy == t->bottom)
    {
        scroll_up(t, t->top, t->bottom, 1);
    }
    else if (t->cy + 1 < t->rows)
    {
        t->cy++;
    }
}

static void put(term_t *t, uint32_t code)
{
    if (t->wrap)
    {
        t->cx = 0;
        line_feed(t);
        t->wrap = 0;
    }

    cell_t *c = &t->grid[t->cy][t->cx];

    c->code = code;
    c->fg = t->fg;
    c->bg = t->bg;
    c->flags = t->flags;
    t->dirty[t->cy] = 1;

    if (t->cx + 1 < t->columns)
    {
        t->cx++;
    }
    else
    {
        t->wrap = 1;
    }
}

static int param(term_t *t, int index, int fallback)
{
    return index < t->param_count && t->params[index] ? t->params[index] : fallback;
}

static void clamp(term_t *t)
{
    if (t->cx >= t->columns)
    {
        t->cx = t->columns - 1;
    }

    if (t->cy >= t->rows)
    {
        t->cy = t->rows - 1;
    }

    if (t->cx < 0)
    {
        t->cx = 0;
    }

    if (t->cy < 0)
    {
        t->cy = 0;
    }
}

static void graphics(term_t *t)
{
    if (t->param_count == 0)
    {
        t->param_count = 1;
        t->params[0] = 0;
    }

    for (int i = 0; i < t->param_count; i++)
    {
        int p = t->params[i];

        if (p == 0)
        {
            t->fg = FG;
            t->bg = BG;
            t->flags = 0;
        }
        else if (p == 1)
        {
            t->flags |= 1;
        }
        else if (p == 7)
        {
            t->flags |= 2;
        }
        else if (p == 22)
        {
            t->flags &= (uint8_t)~1;
        }
        else if (p == 27)
        {
            t->flags &= (uint8_t)~2;
        }
        else if (p >= 30 && p <= 37)
        {
            t->fg = (uint8_t)(p - 30);
        }
        else if (p == 39)
        {
            t->fg = FG;
        }
        else if (p >= 40 && p <= 47)
        {
            t->bg = (uint8_t)(p - 40);
        }
        else if (p == 49)
        {
            t->bg = BG;
        }
        else if (p >= 90 && p <= 97)
        {
            t->fg = (uint8_t)(p - 90 + 8);
        }
        else if (p >= 100 && p <= 107)
        {
            t->bg = (uint8_t)(p - 100 + 8);
        }
        else if ((p == 38 || p == 48) && i + 2 < t->param_count && t->params[i + 1] == 5)
        {
            int n = t->params[i + 2];
            uint8_t color = n < 16 ? (uint8_t)n : n >= 244 ? 15 : n >= 232 ? 8 : 7;

            if (p == 38)
            {
                t->fg = color;
            }
            else
            {
                t->bg = color;
            }

            i += 2;
        }
    }
}

static void reply(term_t *t, const char *text)
{
    pty_write(t->pty, text, strlen(text));
}

static void csi(term_t *t, char final)
{
    int n = param(t, 0, 1);

    t->wrap = 0;

    switch (final)
    {
    case 'm':
        graphics(t);
        break;
    case 'H':
    case 'f':
        t->cy = param(t, 0, 1) - 1;
        t->cx = param(t, 1, 1) - 1;
        break;
    case 'A':
        t->cy -= n;
        break;
    case 'B':
    case 'e':
        t->cy += n;
        break;
    case 'C':
    case 'a':
        t->cx += n;
        break;
    case 'D':
        t->cx -= n;
        break;
    case 'E':
        t->cy += n;
        t->cx = 0;
        break;
    case 'F':
        t->cy -= n;
        t->cx = 0;
        break;
    case 'G':
    case '`':
        t->cx = n - 1;
        break;
    case 'd':
        t->cy = n - 1;
        break;
    case 'J':
    {
        int mode = t->param_count ? t->params[0] : 0;

        if (mode == 0)
        {
            clear_cells(t, t->cy, t->cx, t->columns);

            for (int y = t->cy + 1; y < t->rows; y++)
            {
                clear_cells(t, y, 0, t->columns);
            }
        }
        else if (mode == 1)
        {
            for (int y = 0; y < t->cy; y++)
            {
                clear_cells(t, y, 0, t->columns);
            }

            clear_cells(t, t->cy, 0, t->cx + 1);
        }
        else
        {
            for (int y = 0; y < t->rows; y++)
            {
                clear_cells(t, y, 0, t->columns);
            }
        }

        break;
    }
    case 'K':
    {
        int mode = t->param_count ? t->params[0] : 0;

        clear_cells(t, t->cy, mode == 0 ? t->cx : 0, mode == 1 ? t->cx + 1 : t->columns);
        break;
    }
    case 'X':
        clear_cells(t, t->cy, t->cx, t->cx + n);
        break;
    case 'P':
        for (int x = t->cx; x < t->columns; x++)
        {
            if (x + n < t->columns)
            {
                t->grid[t->cy][x] = t->grid[t->cy][x + n];
            }
            else
            {
                clear_cells(t, t->cy, x, x + 1);
            }
        }

        t->dirty[t->cy] = 1;
        break;
    case '@':
        for (int x = t->columns - 1; x >= t->cx; x--)
        {
            if (x >= t->cx + n)
            {
                t->grid[t->cy][x] = t->grid[t->cy][x - n];
            }
            else
            {
                clear_cells(t, t->cy, x, x + 1);
            }
        }

        t->dirty[t->cy] = 1;
        break;
    case 'L':
        if (t->cy >= t->top && t->cy <= t->bottom)
        {
            scroll_down(t, t->cy, t->bottom, n);
        }

        break;
    case 'M':
        if (t->cy >= t->top && t->cy <= t->bottom)
        {
            scroll_up(t, t->cy, t->bottom, n);
        }

        break;
    case 'S':
        scroll_up(t, t->top, t->bottom, n);
        break;
    case 'T':
        scroll_down(t, t->top, t->bottom, n);
        break;
    case 'r':
    {
        int top = param(t, 0, 1) - 1;
        int bottom = param(t, 1, t->rows) - 1;

        if (top < bottom && bottom < t->rows)
        {
            t->top = top;
            t->bottom = bottom;
        }
        else
        {
            t->top = 0;
            t->bottom = t->rows - 1;
        }

        t->cx = 0;
        t->cy = 0;
        break;
    }
    case 's':
        t->saved_x = t->cx;
        t->saved_y = t->cy;
        break;
    case 'u':
        t->cx = t->saved_x;
        t->cy = t->saved_y;
        break;
    case 'n':
        if (t->param_count && t->params[0] == 6)
        {
            char text[32];

            clamp(t);
            snprintf(text, sizeof(text), "\033[%d;%dR", t->cy + 1, t->cx + 1);
            reply(t, text);
        }

        break;
    case 'h':
    case 'l':
        if (t->private_mode)
        {
            for (int i = 0; i < t->param_count; i++)
            {
                if (t->params[i] == 25)
                {
                    t->cursor_on = final == 'h';
                }
                else if (t->params[i] == 1049 || t->params[i] == 47 || t->params[i] == 1047)
                {
                    for (int y = 0; y < t->rows; y++)
                    {
                        clear_cells(t, y, 0, t->columns);
                    }

                    t->cx = 0;
                    t->cy = 0;
                }
            }
        }

        break;
    default:
        break;
    }

    clamp(t);
}

static void feed(term_t *t, unsigned char c)
{
    if (t->state == 3)
    {
        t->state = 0;
        return;
    }

    if (t->state == 1)
    {
        t->state = 0;

        if (c == '[')
        {
            t->state = 2;
            t->param_count = 0;
            t->private_mode = 0;
            memset(t->params, 0, sizeof(t->params));
        }
        else if (c == '(' || c == ')')
        {
            t->state = 3;
        }
        else if (c == '7')
        {
            t->saved_x = t->cx;
            t->saved_y = t->cy;
        }
        else if (c == '8')
        {
            t->cx = t->saved_x;
            t->cy = t->saved_y;
        }
        else if (c == 'M')
        {
            if (t->cy == t->top)
            {
                scroll_down(t, t->top, t->bottom, 1);
            }
            else if (t->cy > 0)
            {
                t->cy--;
            }
        }
        else if (c == 'c')
        {
            t->fg = FG;
            t->bg = BG;
            t->flags = 0;

            for (int y = 0; y < t->rows; y++)
            {
                clear_cells(t, y, 0, t->columns);
            }

            t->cx = 0;
            t->cy = 0;
        }

        return;
    }

    if (t->state == 2)
    {
        if (c == '?' || c == '>')
        {
            t->private_mode = 1;
        }
        else if (c >= '0' && c <= '9')
        {
            if (t->param_count == 0)
            {
                t->param_count = 1;
            }

            if (t->param_count <= PARAMS_MAX)
            {
                t->params[t->param_count - 1] = t->params[t->param_count - 1] * 10 + (c - '0');
            }
        }
        else if (c == ';')
        {
            if (t->param_count == 0)
            {
                t->param_count = 1;
            }

            if (t->param_count < PARAMS_MAX)
            {
                t->param_count++;
            }
        }
        else if (c >= 0x40 && c <= 0x7E)
        {
            t->state = 0;
            csi(t, (char)c);
        }

        return;
    }

    if (t->utf8_left)
    {
        if ((c & 0xC0) == 0x80)
        {
            t->utf8 = (t->utf8 << 6) | (c & 0x3F);

            if (--t->utf8_left == 0)
            {
                put(t, t->utf8);
            }

            return;
        }

        t->utf8_left = 0;
    }

    if (c >= 0xC0 && c < 0xF8)
    {
        t->utf8_left = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
        t->utf8 = c & (c >= 0xF0 ? 0x07 : c >= 0xE0 ? 0x0F : 0x1F);
        return;
    }

    switch (c)
    {
    case 27:
        t->state = 1;
        break;
    case '\n':
        t->cx = 0;
        t->wrap = 0;
        line_feed(t);
        break;
    case '\r':
        t->cx = 0;
        t->wrap = 0;
        break;
    case '\b':
        if (t->cx > 0)
        {
            t->cx--;
        }

        t->wrap = 0;
        break;
    case '\t':
        t->cx = (t->cx + 8) & ~7;
        clamp(t);
        break;
    default:
        if (c >= 32 && c != 127)
        {
            put(t, c);
        }

        break;
    }
}

static void layout(window_t *w, term_t *t)
{
    int columns = (w->content.width - 16) / t->cell_w;
    int rows = (w->content.height - 12) / t->cell_h;

    columns = columns < 10 ? 10 : columns > TERM_COLUMNS_MAX ? TERM_COLUMNS_MAX : columns;
    rows = rows < 4 ? 4 : rows > TERM_ROWS_MAX ? TERM_ROWS_MAX : rows;

    if (columns == t->columns && rows == t->rows)
    {
        return;
    }

    for (int y = 0; y < TERM_ROWS_MAX; y++)
    {
        for (int x = (y < t->rows ? t->columns : 0); x < TERM_COLUMNS_MAX; x++)
        {
            t->grid[y][x].code = ' ';
            t->grid[y][x].fg = FG;
            t->grid[y][x].bg = BG;
            t->grid[y][x].flags = 0;
        }
    }

    if (t->rows && rows < t->rows && t->cy >= rows)
    {
        int shift = t->cy - rows + 1;

        for (int y = 0; y + shift < t->rows; y++)
        {
            memcpy(t->grid[y], t->grid[y + shift], sizeof(t->grid[y]));
        }

        t->cy -= shift;
    }

    t->columns = columns;
    t->rows = rows;
    t->top = 0;
    t->bottom = rows - 1;
    clamp(t);
    t->full = 1;
}

static int term_open(window_t *w, const char *arg)
{
    term_t *t = calloc(1, sizeof(term_t));

    if (!t)
    {
        return -1;
    }

    t->cell_w = text_width(font_mono, S(13), "M");
    t->cell_h = SI(19);
    t->fg = FG;
    t->bg = BG;
    t->cursor_on = 1;
    t->pty = pty_open();

    if (t->pty < 0)
    {
        free(t);
        notify("Terminal", "No terminal is free", "Close a terminal window first");
        return -1;
    }

    w->state = t;
    layout(w, t);

    const char *program = arg && arg[0] && arg[0] != '!' ? arg : "knocsh";

    t->pid = pty_spawn(t->pty, program, "");

    if (arg && arg[0] == '!' && t->pid >= 0)
    {
        pty_write(t->pty, arg + 1, strlen(arg + 1));
        pty_write(t->pty, "\r", 1);
    }

    if (t->pid < 0)
    {
        const char *text = "Could not start the program.\r\n";

        for (const char *p = text; *p; p++)
        {
            feed(t, (unsigned char)*p);
        }

        t->ended = 1;
    }

    const char *name = strrchr(program, '/');

    window_set_title(w, "Terminal", name ? name + 1 : program);
    return 0;
}

static void draw_row(term_t *t, canvas_t *c, int y)
{
    int py = 6 + y * t->cell_h;
    int baseline = py + t->cell_h - SI(5);

    draw_fill(c, 8, py, t->columns * t->cell_w, t->cell_h, T.term);

    for (int x = 0; x < t->columns; x++)
    {
        cell_t *cell = &t->grid[y][x];
        uint8_t fg = cell->fg;
        uint8_t bg = cell->bg;

        if ((cell->flags & 1) && fg < 8)
        {
            fg = (uint8_t)(fg + 8);
        }

        uint32_t front = color_of(fg);
        uint32_t back = color_of(bg);

        if (cell->flags & 2)
        {
            uint32_t swap = front;

            front = back;
            back = swap;
        }

        if (back != T.term)
        {
            draw_fill(c, 8 + x * t->cell_w, py, t->cell_w, t->cell_h, back);
        }

        if (cell->code > ' ')
        {
            char text[5] = {0};
            uint32_t code = cell->code;

            if (code < 0x80)
            {
                text[0] = (char)code;
            }
            else if (code < 0x800)
            {
                text[0] = (char)(0xC0 | (code >> 6));
                text[1] = (char)(0x80 | (code & 0x3F));
            }
            else
            {
                text[0] = (char)(0xE0 | (code >> 12));
                text[1] = (char)(0x80 | ((code >> 6) & 0x3F));
                text[2] = (char)(0x80 | (code & 0x3F));
            }

            draw_text(c, font_mono, S(13), 8 + x * t->cell_w, baseline, text, front);
        }
    }

    if (t->cursor_on && y == t->cy && !t->ended)
    {
        draw_fill(c, 8 + t->cx * t->cell_w, py + t->cell_h - 3, t->cell_w, 2, T.a);
    }
}

static void term_draw(window_t *w, canvas_t *c)
{
    term_t *t = w->state;

    layout(w, t);

    if (c->pixels != t->seen_pixels)
    {
        t->seen_pixels = c->pixels;
        t->full = 1;
    }

    if (t->drawn_cy != t->cy && t->drawn_cy < t->rows)
    {
        t->dirty[t->drawn_cy] = 1;
    }

    t->drawn_cy = t->cy;

    if (t->full)
    {
        draw_fill(c, 0, 0, c->width, c->height, T.term);

        for (int y = 0; y < t->rows; y++)
        {
            t->dirty[y] = 1;
        }

        t->full = 0;
    }

    for (int y = 0; y < t->rows; y++)
    {
        if (t->dirty[y])
        {
            draw_row(t, c, y);
            t->dirty[y] = 0;
        }
    }
}

static void send(term_t *t, const char *text)
{
    pty_write(t->pty, text, strlen(text));
}

static void term_event(window_t *w, const input_event_t *e, int x, int y)
{
    term_t *t = w->state;

    (void)x;
    (void)y;

    if (e->type != INPUT_KEY || e->value == 0)
    {
        return;
    }

    if (t->ended)
    {
        window_close(w);
        return;
    }

    switch (e->code)
    {
    case KEY_UP:
        send(t, "\033[A");
        return;
    case KEY_DOWN:
        send(t, "\033[B");
        return;
    case KEY_RIGHT:
        send(t, "\033[C");
        return;
    case KEY_LEFT:
        send(t, "\033[D");
        return;
    case KEY_HOME:
        send(t, "\033[H");
        return;
    case KEY_END:
        send(t, "\033[F");
        return;
    case KEY_DELETE:
        send(t, "\033[3~");
        return;
    case KEY_PAGEUP:
        send(t, "\033[5~");
        return;
    case KEY_PAGEDOWN:
        send(t, "\033[6~");
        return;
    default:
        break;
    }

    uint32_t c = e->text;

    if (!c)
    {
        return;
    }

    if (e->modifiers & INPUT_CTRL)
    {
        if ((c | 0x20) >= 'a' && (c | 0x20) <= 'z')
        {
            c = (c | 0x20) - 'a' + 1;
        }
        else if (c == '[')
        {
            c = 27;
        }
    }

    char ch = (char)c;

    pty_write(t->pty, &ch, 1);
}

static int process_running(int pid)
{
    process_info_t info;

    for (unsigned long i = 0; ps(i, &info) == 0; i++)
    {
        if (info.pid == pid && info.state != 0 && info.state != 4 && info.state != 5)
        {
            return 1;
        }
    }

    return 0;
}

static void term_tick(window_t *w)
{
    term_t *t = w->state;
    char chunk[2048];
    int changed = 0;

    for (int round = 0; round < 4; round++)
    {
        long n = pty_read(t->pty, chunk, sizeof(chunk));

        if (n <= 0)
        {
            break;
        }

        for (long i = 0; i < n; i++)
        {
            feed(t, (unsigned char)chunk[i]);
        }

        changed = 1;
    }

    if (!t->ended && t->pid > 0 && uptime() - t->last_check > 50)
    {
        t->last_check = uptime();

        if (!process_running(t->pid))
        {
            const char *text = "\r\n[the program ended: press a key to close]\r\n";

            for (const char *p = text; *p; p++)
            {
                feed(t, (unsigned char)*p);
            }

            t->ended = 1;
            changed = 1;
        }
    }

    if (changed)
    {
        t->dirty[t->cy] = 1;
        window_dirty(w);
    }
}

static void term_close(window_t *w)
{
    term_t *t = w->state;

    if (t->pid > 0 && process_running(t->pid))
    {
        kill(t->pid);
    }

    free(t);
    w->state = 0;
}

const app_t app_terminal = {"Terminal", 720, 440, term_open, term_draw, term_event, term_tick, term_close};
