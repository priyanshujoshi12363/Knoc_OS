#include "fbcon.h"
#include "virtio_gpu.h"
#include "spinlock.h"
#include "tty.h"
#include "console_font.h"
#include "splash_font.h"
#include "timer.h"

#define MAX_COLUMNS 256
#define MAX_ROWS 128
#define MARGIN_X 16
#define MARGIN_Y 12
#define EARLY_SIZE 16384
#define PARAMS_MAX 8
#define TAG_MAX 12

#define COLOR_FG 16
#define COLOR_BG 17
#define COLOR_DIM 18
#define COLOR_ACCENT 19
#define COLOR_TAG 20
#define COLOR_CURSOR 21

#define FLAG_BOLD 1
#define FLAG_REVERSE 2

typedef struct cell
{
    uint32_t code;
    uint8_t fg;
    uint8_t bg;
    uint8_t flags;
    uint8_t unused;
} cell_t;

enum
{
    STATE_TEXT,
    STATE_ESCAPE,
    STATE_CSI,
    STATE_CHARSET,
};

static const uint32_t palette[] = {
    0x2A2E33, 0xEE6B5C, 0x5DBE8A, 0xE9C46A, 0x7F9CC8, 0xC792B9, 0x6FB8B0, 0xC9CDD0,
    0x6C737A, 0xFF8A7C, 0x7FD6A4, 0xF4D68A, 0xA3BCE0, 0xDDB0D2, 0x8FD4CC, 0xECEDEE,
    0xD6DADC, 0x121416, 0x6C737A, 0xF08A5D, 0xA0A6AC, 0xECEDEE,
};

static cell_t grid[MAX_ROWS][MAX_COLUMNS];
static uint8_t dirty[MAX_ROWS];
static uint32_t columns;
static uint32_t rows;
static uint32_t cursor_x;
static uint32_t cursor_y;
static uint32_t saved_x;
static uint32_t saved_y;
static uint32_t drawn_cursor_x;
static uint32_t drawn_cursor_y;
static uint32_t region_top;
static uint32_t region_bottom;
static uint32_t scroll_pending;
static int wrap_pending;
static int cursor_visible = 1;
static uint8_t fg = COLOR_FG;
static uint8_t bg = COLOR_BG;
static uint8_t flags;
static int state;
static uint32_t params[PARAMS_MAX];
static uint32_t param_count;
static int private_mode;
static uint32_t utf8_code;
static int utf8_left;
static int tag_open;
static uint32_t tag_x;
static int active;
static int paused;
static int redraw_all;
static int splash;
static uint32_t splash_lines;
static uint64_t splash_since;
static uint64_t splash_drawn_at;
static uint32_t splash_drawn_lines;
static char status_line[MAX_COLUMNS];
static char status_shown[MAX_COLUMNS];
static uint32_t status_length;
static int status_capture;
static uint32_t column_seen;
static char early[EARLY_SIZE];
static uint32_t early_length;
static spinlock_t lock = SPINLOCK_INIT;

static void mark(uint32_t row)
{
    if (row < rows)
    {
        dirty[row] = 1;
    }
}

static void clear_cells(uint32_t row, uint32_t from, uint32_t to)
{
    for (uint32_t x = from; x < to && x < columns; x++)
    {
        grid[row][x].code = ' ';
        grid[row][x].fg = fg;
        grid[row][x].bg = bg;
        grid[row][x].flags = 0;
    }

    mark(row);
}

static void copy_row(uint32_t to, uint32_t from)
{
    for (uint32_t x = 0; x < columns; x++)
    {
        grid[to][x] = grid[from][x];
    }

    mark(to);
}

static void scroll_up(uint32_t top, uint32_t bottom, uint32_t count)
{
    for (uint32_t n = 0; n < count; n++)
    {
        for (uint32_t y = top; y < bottom; y++)
        {
            copy_row(y, y + 1);
        }

        clear_cells(bottom, 0, columns);
    }

    if (top == 0 && bottom == rows - 1)
    {
        scroll_pending += count;

        for (uint32_t y = 0; y < rows; y++)
        {
            dirty[y] = y + count < rows ? dirty[y + count] : 1;
        }
    }
}

static void scroll_down(uint32_t top, uint32_t bottom, uint32_t count)
{
    for (uint32_t n = 0; n < count; n++)
    {
        for (uint32_t y = bottom; y > top; y--)
        {
            copy_row(y, y - 1);
        }

        clear_cells(top, 0, columns);
    }
}

static void line_feed(void)
{
    if (cursor_y == region_bottom)
    {
        scroll_up(region_top, region_bottom, 1);
    }
    else if (cursor_y + 1 < rows)
    {
        cursor_y++;
    }
}

static uint32_t tag_color(void)
{
    char tag[TAG_MAX + 1];
    uint32_t n = 0;

    for (uint32_t x = tag_x + 1; x < cursor_x && n < TAG_MAX; x++)
    {
        tag[n++] = (char)grid[cursor_y][x].code;
    }

    tag[n] = 0;

    static const char *const red[] = {"WARN", "ERROR", "OOPS", "PANIC", "SECURITY", "FAIL", 0};
    static const char *const accent[] = {"AI", "GUARDIAN", "HEALTH", "KNOC", "AGENT", 0};

    for (int i = 0; red[i]; i++)
    {
        const char *a = red[i];
        uint32_t j = 0;

        while (a[j] && a[j] == tag[j])
        {
            j++;
        }

        if (!a[j] && !tag[j])
        {
            return i == 0 ? 3 : 1;
        }
    }

    for (int i = 0; accent[i]; i++)
    {
        const char *a = accent[i];
        uint32_t j = 0;

        while (a[j] && a[j] == tag[j])
        {
            j++;
        }

        if (!a[j] && !tag[j])
        {
            return COLOR_ACCENT;
        }
    }

    if (tag[0] == 'I' && tag[1] == 'N' && tag[2] == 'F' && tag[3] == 'O' && !tag[4])
    {
        return COLOR_DIM;
    }

    return COLOR_TAG;
}

static void put_code(uint32_t code)
{
    if (wrap_pending)
    {
        cursor_x = 0;
        line_feed();
        wrap_pending = 0;
    }

    if (code == '[' && cursor_x == 0 && fg == COLOR_FG && bg == COLOR_BG && !flags)
    {
        tag_open = 1;
        tag_x = 0;
    }

    cell_t *c = &grid[cursor_y][cursor_x];

    c->code = code;
    c->fg = fg;
    c->bg = bg;
    c->flags = flags;
    mark(cursor_y);

    if (tag_open && code == ']')
    {
        uint32_t color = tag_color();

        for (uint32_t x = tag_x; x <= cursor_x; x++)
        {
            grid[cursor_y][x].fg = (uint8_t)color;
        }

        tag_open = 0;
    }
    else if (tag_open && (cursor_x - tag_x > TAG_MAX || code == ' '))
    {
        tag_open = 0;
    }

    if (cursor_x + 1 < columns)
    {
        cursor_x++;
    }
    else
    {
        wrap_pending = 1;
    }
}

static uint32_t param(uint32_t index, uint32_t fallback)
{
    return index < param_count && params[index] ? params[index] : fallback;
}

static void set_graphics(void)
{
    if (param_count == 0)
    {
        param_count = 1;
        params[0] = 0;
    }

    for (uint32_t i = 0; i < param_count; i++)
    {
        uint32_t p = params[i];

        if (p == 0)
        {
            fg = COLOR_FG;
            bg = COLOR_BG;
            flags = 0;
        }
        else if (p == 1)
        {
            flags |= FLAG_BOLD;
        }
        else if (p == 7)
        {
            flags |= FLAG_REVERSE;
        }
        else if (p == 22)
        {
            flags &= ~FLAG_BOLD;
        }
        else if (p == 27)
        {
            flags &= ~FLAG_REVERSE;
        }
        else if (p >= 30 && p <= 37)
        {
            fg = (uint8_t)(p - 30);
        }
        else if (p == 39)
        {
            fg = COLOR_FG;
        }
        else if (p >= 40 && p <= 47)
        {
            bg = (uint8_t)(p - 40);
        }
        else if (p == 49)
        {
            bg = COLOR_BG;
        }
        else if (p >= 90 && p <= 97)
        {
            fg = (uint8_t)(p - 90 + 8);
        }
        else if (p >= 100 && p <= 107)
        {
            bg = (uint8_t)(p - 100 + 8);
        }
        else if ((p == 38 || p == 48) && i + 2 < param_count && params[i + 1] == 5)
        {
            uint32_t n = params[i + 2];
            uint8_t color = n < 16 ? (uint8_t)n : n >= 244 ? 15 : n >= 232 ? 8 : 7;

            if (p == 38)
            {
                fg = color;
            }
            else
            {
                bg = color;
            }

            i += 2;
        }
    }
}

static void reply(const char *text)
{
    while (*text)
    {
        tty_input(*text++);
    }
}

static void reply_position(void)
{
    char text[24];
    uint32_t n = 0;
    uint32_t values[2] = {cursor_y + 1, cursor_x + 1};

    text[n++] = 27;
    text[n++] = '[';

    for (int v = 0; v < 2; v++)
    {
        char digits[10];
        int d = 0;
        uint32_t value = values[v];

        do
        {
            digits[d++] = (char)('0' + value % 10);
            value /= 10;
        } while (value);

        while (d)
        {
            text[n++] = digits[--d];
        }

        text[n++] = v == 0 ? ';' : 'R';
    }

    text[n] = 0;
    reply(text);
}

static void clamp_cursor(void)
{
    if (cursor_x >= columns)
    {
        cursor_x = columns - 1;
    }

    if (cursor_y >= rows)
    {
        cursor_y = rows - 1;
    }
}

static void csi(char final)
{
    uint32_t n = param(0, 1);

    wrap_pending = 0;

    switch (final)
    {
    case 'm':
        set_graphics();
        break;
    case 'H':
    case 'f':
        cursor_y = param(0, 1) - 1;
        cursor_x = param(1, 1) - 1;
        break;
    case 'A':
        cursor_y = cursor_y > n ? cursor_y - n : 0;
        break;
    case 'B':
    case 'e':
        cursor_y += n;
        break;
    case 'C':
    case 'a':
        cursor_x += n;
        break;
    case 'D':
        cursor_x = cursor_x > n ? cursor_x - n : 0;
        break;
    case 'E':
        cursor_y += n;
        cursor_x = 0;
        break;
    case 'F':
        cursor_y = cursor_y > n ? cursor_y - n : 0;
        cursor_x = 0;
        break;
    case 'G':
    case '`':
        cursor_x = n - 1;
        break;
    case 'd':
        cursor_y = n - 1;
        break;
    case 'J':
    {
        uint32_t mode = param_count ? params[0] : 0;

        if (mode == 0)
        {
            clear_cells(cursor_y, cursor_x, columns);

            for (uint32_t y = cursor_y + 1; y < rows; y++)
            {
                clear_cells(y, 0, columns);
            }
        }
        else if (mode == 1)
        {
            for (uint32_t y = 0; y < cursor_y; y++)
            {
                clear_cells(y, 0, columns);
            }

            clear_cells(cursor_y, 0, cursor_x + 1);
        }
        else
        {
            for (uint32_t y = 0; y < rows; y++)
            {
                clear_cells(y, 0, columns);
            }
        }

        break;
    }
    case 'K':
    {
        uint32_t mode = param_count ? params[0] : 0;

        clear_cells(cursor_y, mode == 0 ? cursor_x : 0, mode == 1 ? cursor_x + 1 : columns);
        break;
    }
    case 'X':
        clear_cells(cursor_y, cursor_x, cursor_x + n);
        break;
    case 'P':
        for (uint32_t x = cursor_x; x < columns; x++)
        {
            if (x + n < columns)
            {
                grid[cursor_y][x] = grid[cursor_y][x + n];
            }
            else
            {
                grid[cursor_y][x].code = ' ';
                grid[cursor_y][x].fg = fg;
                grid[cursor_y][x].bg = bg;
                grid[cursor_y][x].flags = 0;
            }
        }

        mark(cursor_y);
        break;
    case '@':
        for (uint32_t x = columns; x-- > cursor_x;)
        {
            if (x >= cursor_x + n)
            {
                grid[cursor_y][x] = grid[cursor_y][x - n];
            }
            else
            {
                grid[cursor_y][x].code = ' ';
                grid[cursor_y][x].fg = fg;
                grid[cursor_y][x].bg = bg;
                grid[cursor_y][x].flags = 0;
            }
        }

        mark(cursor_y);
        break;
    case 'L':
        if (cursor_y >= region_top && cursor_y <= region_bottom)
        {
            scroll_down(cursor_y, region_bottom, n);
        }

        break;
    case 'M':
        if (cursor_y >= region_top && cursor_y <= region_bottom)
        {
            scroll_up(cursor_y, region_bottom, n);
        }

        break;
    case 'S':
        scroll_up(region_top, region_bottom, n);
        break;
    case 'T':
        scroll_down(region_top, region_bottom, n);
        break;
    case 'r':
    {
        uint32_t top = param(0, 1) - 1;
        uint32_t bottom = param(1, rows) - 1;

        if (top < bottom && bottom < rows)
        {
            region_top = top;
            region_bottom = bottom;
        }
        else
        {
            region_top = 0;
            region_bottom = rows - 1;
        }

        cursor_x = 0;
        cursor_y = 0;
        break;
    }
    case 's':
        saved_x = cursor_x;
        saved_y = cursor_y;
        break;
    case 'u':
        cursor_x = saved_x;
        cursor_y = saved_y;
        break;
    case 'n':
        if (param_count && params[0] == 6)
        {
            clamp_cursor();
            reply_position();
        }
        else if (param_count && params[0] == 5)
        {
            reply("\033[0n");
        }

        break;
    case 'h':
    case 'l':
        if (private_mode)
        {
            for (uint32_t i = 0; i < param_count; i++)
            {
                if (params[i] == 25)
                {
                    cursor_visible = final == 'h';
                }
                else if (params[i] == 1049 || params[i] == 47 || params[i] == 1047)
                {
                    for (uint32_t y = 0; y < rows; y++)
                    {
                        clear_cells(y, 0, columns);
                    }

                    cursor_x = 0;
                    cursor_y = 0;
                }
            }
        }

        break;
    default:
        break;
    }

    clamp_cursor();
}

static void put_byte(uint8_t c)
{
    if (state == STATE_CHARSET)
    {
        state = STATE_TEXT;
        return;
    }

    if (state == STATE_ESCAPE)
    {
        state = STATE_TEXT;

        if (c == '[')
        {
            state = STATE_CSI;
            param_count = 0;
            private_mode = 0;

            for (int i = 0; i < PARAMS_MAX; i++)
            {
                params[i] = 0;
            }
        }
        else if (c == '(' || c == ')')
        {
            state = STATE_CHARSET;
        }
        else if (c == '7')
        {
            saved_x = cursor_x;
            saved_y = cursor_y;
        }
        else if (c == '8')
        {
            cursor_x = saved_x;
            cursor_y = saved_y;
        }
        else if (c == 'M')
        {
            if (cursor_y == region_top)
            {
                scroll_down(region_top, region_bottom, 1);
            }
            else if (cursor_y > 0)
            {
                cursor_y--;
            }
        }
        else if (c == 'D')
        {
            line_feed();
        }
        else if (c == 'E')
        {
            cursor_x = 0;
            line_feed();
        }
        else if (c == 'c')
        {
            fg = COLOR_FG;
            bg = COLOR_BG;
            flags = 0;
            region_top = 0;
            region_bottom = rows - 1;

            for (uint32_t y = 0; y < rows; y++)
            {
                clear_cells(y, 0, columns);
            }

            cursor_x = 0;
            cursor_y = 0;
        }

        return;
    }

    if (state == STATE_CSI)
    {
        if (c == '?' || c == '>')
        {
            private_mode = 1;
        }
        else if (c >= '0' && c <= '9')
        {
            if (param_count == 0)
            {
                param_count = 1;
            }

            if (param_count <= PARAMS_MAX)
            {
                params[param_count - 1] = params[param_count - 1] * 10 + (c - '0');
            }
        }
        else if (c == ';')
        {
            if (param_count == 0)
            {
                param_count = 1;
            }

            if (param_count < PARAMS_MAX)
            {
                param_count++;
            }
        }
        else if (c >= 0x40 && c <= 0x7E)
        {
            state = STATE_TEXT;
            csi((char)c);
        }

        return;
    }

    if (utf8_left)
    {
        if ((c & 0xC0) == 0x80)
        {
            utf8_code = (utf8_code << 6) | (c & 0x3F);

            if (--utf8_left == 0)
            {
                put_code(utf8_code);
            }

            return;
        }

        utf8_left = 0;
        put_code('?');
    }

    if (c >= 0xC0 && c < 0xF8)
    {
        utf8_left = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
        utf8_code = c & (c >= 0xF0 ? 0x07 : c >= 0xE0 ? 0x0F : 0x1F);
        return;
    }

    if (splash)
    {
        if (c == '\n')
        {
            int keys = 0;

            for (uint32_t i = 0; i + 4 < status_length; i++)
            {
                keys |= status_line[i] == 'C' && status_line[i + 1] == 't' && status_line[i + 2] == 'r' && status_line[i + 3] == 'l';
            }

            if (status_capture && status_length && !keys)
            {
                for (uint32_t i = 0; i <= status_length && i < MAX_COLUMNS; i++)
                {
                    status_shown[i] = i < status_length ? status_line[i] : 0;
                }
            }

            splash_lines++;
            status_length = 0;
            status_capture = 0;
            column_seen = 0;
        }
        else if (c >= 32 && c < 127)
        {
            if (column_seen < 7)
            {
                if (column_seen == 0)
                {
                    status_capture = 1;
                }

                if ((char)c != "[INFO] "[column_seen])
                {
                    status_capture = 0;
                }
            }
            else if (status_capture && status_length < 90)
            {
                status_line[status_length++] = (char)c;
            }

            column_seen++;
        }
    }

    switch (c)
    {
    case 27:
        state = STATE_ESCAPE;
        break;
    case '\n':
        cursor_x = 0;
        wrap_pending = 0;
        tag_open = 0;
        line_feed();
        break;
    case '\r':
        cursor_x = 0;
        wrap_pending = 0;
        break;
    case '\b':
        if (cursor_x > 0)
        {
            cursor_x--;
        }

        wrap_pending = 0;
        break;
    case '\t':
        cursor_x = (cursor_x + 8) & ~7U;

        if (cursor_x >= columns)
        {
            cursor_x = columns - 1;
        }

        break;
    default:
        if (c >= 32 && c != 127)
        {
            put_code(c);
        }

        break;
    }
}

static const uint8_t *glyph(uint32_t code)
{
    if (code >= 32 && code < 127)
    {
        return console_font_alpha[code - 32];
    }

    for (uint32_t i = 95; i < CONSOLE_FONT_COUNT; i++)
    {
        if (console_font_codes[i] == code)
        {
            return console_font_alpha[i];
        }
    }

    return code == 0x00A0 ? console_font_alpha[0] : console_font_alpha['?' - 32];
}

static uint32_t blend(uint32_t back, uint32_t front, uint32_t alpha)
{
    uint32_t out = 0;

    for (int shift = 0; shift <= 16; shift += 8)
    {
        int b = (int)((back >> shift) & 0xFF);
        int f = (int)((front >> shift) & 0xFF);

        out |= (uint32_t)(b + ((f - b) * (int)alpha + 127) / 255) << shift;
    }

    return out;
}

static void draw_cell(uint32_t row, uint32_t column, int with_cursor)
{
    uint32_t *fb = gpu_framebuffer();
    uint32_t stride = gpu_width();
    cell_t *c = &grid[row][column];
    uint8_t f = c->fg;
    uint8_t b = c->bg;

    if ((c->flags & FLAG_BOLD) && f < 8)
    {
        f = (uint8_t)(f + 8);
    }

    uint32_t front = palette[f];
    uint32_t back = palette[b];

    if (c->flags & FLAG_REVERSE)
    {
        uint32_t t = front;

        front = back;
        back = t;
    }

    const uint8_t *alpha = glyph(c->code);
    uint32_t *line = fb + (MARGIN_Y + row * CONSOLE_FONT_H) * stride + MARGIN_X + column * CONSOLE_FONT_W;

    for (uint32_t y = 0; y < CONSOLE_FONT_H; y++)
    {
        for (uint32_t x = 0; x < CONSOLE_FONT_W; x++)
        {
            uint32_t a = alpha[y * CONSOLE_FONT_W + x];

            line[x] = a == 0 ? back : a == 255 ? front : blend(back, front, a);
        }

        if (with_cursor && y >= CONSOLE_FONT_H - 3)
        {
            for (uint32_t x = 0; x < CONSOLE_FONT_W; x++)
            {
                line[x] = palette[COLOR_CURSOR];
            }
        }

        line += stride;
    }
}

static void fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
    uint32_t *fb = gpu_framebuffer();
    uint32_t stride = gpu_width();

    for (uint32_t j = y; j < y + h; j++)
    {
        for (uint32_t i = x; i < x + w; i++)
        {
            fb[j * stride + i] = color;
        }
    }
}

static void move_pixels_up(uint32_t lines)
{
    uint64_t *fb = (uint64_t *)gpu_framebuffer();
    uint32_t stride_words = gpu_width() / 2;
    uint32_t top = MARGIN_Y;
    uint32_t height = rows * CONSOLE_FONT_H;
    uint32_t shift = lines * CONSOLE_FONT_H;

    for (uint32_t y = top; y + shift < top + height; y++)
    {
        uint64_t *to = fb + (uint64_t)y * stride_words;
        uint64_t *from = fb + (uint64_t)(y + shift) * stride_words;

        for (uint32_t x = 0; x < stride_words; x++)
        {
            to[x] = from[x];
        }
    }
}

static uint32_t splash_back(uint32_t x, uint32_t y)
{
    uint32_t w = gpu_width();
    uint32_t h = gpu_height();
    int dx = (int)x - (int)w / 2;
    int dy = (int)y - (int)(h * 45 / 100);
    uint64_t d2 = (uint64_t)(dx * dx + dy * dy);
    uint64_t r2 = (uint64_t)(w / 2) * (w / 2);

    if (d2 >= r2)
    {
        return palette[COLOR_BG];
    }

    uint32_t t = (uint32_t)((r2 - d2) * 1024 / r2);
    uint32_t alpha = t * t * 40 / (1024 * 1024 / 16);
    uint32_t hash = (x * 73856093u) ^ (y * 19349663u);

    alpha = alpha + (hash & 15);
    return blend(palette[COLOR_BG], palette[COLOR_ACCENT], alpha / 16);
}

static void splash_text(uint32_t *fb, uint32_t stride, uint32_t x, uint32_t y, const char *text, uint32_t color)
{
    for (const char *p = text; *p; p++)
    {
        uint32_t c = (uint8_t)*p;

        if (c < 32 || c > 126)
        {
            continue;
        }

        const uint8_t *alpha = splash_font_alpha[c - 32];

        for (uint32_t j = 0; j < SPLASH_FONT_H; j++)
        {
            uint32_t *row = fb + (y + j) * stride + x;

            for (uint32_t i = 0; i < SPLASH_FONT_W; i++)
            {
                uint32_t a = alpha[j * SPLASH_FONT_W + i];

                if (a)
                {
                    row[i] = blend(row[i], color, a);
                }
            }
        }

        x += splash_font_advance[c - 32];
    }
}

static uint32_t splash_width(const char *text)
{
    uint32_t w = 0;

    for (const char *p = text; *p; p++)
    {
        w += (uint8_t)*p >= 32 && (uint8_t)*p < 127 ? splash_font_advance[(uint8_t)*p - 32] : 0;
    }

    return w;
}

static void small_text(uint32_t *fb, uint32_t stride, uint32_t x, uint32_t y, const char *text, uint32_t color)
{
    for (const char *p = text; *p; p++, x += CONSOLE_FONT_W)
    {
        const uint8_t *alpha = glyph((uint8_t)*p);

        for (uint32_t j = 0; j < CONSOLE_FONT_H; j++)
        {
            for (uint32_t i = 0; i < CONSOLE_FONT_W; i++)
            {
                uint32_t a = alpha[j * CONSOLE_FONT_W + i];
                uint32_t back = splash_back(x + i, y + j);

                fb[(y + j) * stride + x + i] = a ? blend(back, color, a) : back;
            }
        }
    }
}

static void render_splash(void)
{
    uint32_t *fb = gpu_framebuffer();
    uint32_t w = gpu_width();
    uint32_t h = gpu_height();
    uint32_t accent = palette[COLOR_ACCENT];
    uint64_t now = timer_ticks();
    int first = splash_drawn_at == 0;

    if (first)
    {
        for (uint32_t y = 0; y < h; y++)
        {
            for (uint32_t x = 0; x < w; x++)
            {
                fb[y * w + x] = splash_back(x, y);
            }
        }
    }

    uint32_t cx = w / 2;
    uint32_t top = h * 40 / 100;
    const char *word = "KnocOS";
    uint32_t ww = splash_width(word);

    if (first)
    {
        splash_text(fb, w, cx - ww / 2, top, word, palette[COLOR_FG] | 0x101010);
    }

    uint32_t sq = 9;
    uint32_t gap = 5;
    uint32_t sx = cx - sq - gap / 2;
    uint32_t sy = top - 36;

    for (int k = 0; k < 4; k++)
    {
        int phase = (int)((now / 4 + (uint64_t)k * 5) % 16);
        uint32_t alpha = 90 + (uint32_t)(phase < 8 ? phase : 16 - phase) * 20;
        uint32_t x0 = sx + (uint32_t)(k % 2) * (sq + gap);
        uint32_t y0 = sy + (uint32_t)(k / 2) * (sq + gap);

        for (uint32_t j = 0; j < sq; j++)
        {
            for (uint32_t i = 0; i < sq; i++)
            {
                fb[(y0 + j) * w + x0 + i] = blend(splash_back(x0 + i, y0 + j), accent, alpha);
            }
        }
    }

    uint32_t bar_w = 240;
    uint32_t bar_y = top + SPLASH_FONT_H + 34;
    uint32_t bar_x = cx - bar_w / 2;
    uint32_t progress = splash_lines * bar_w / 120;

    progress = progress > bar_w - 12 ? bar_w - 12 : progress;

    for (uint32_t j = 0; j < 3; j++)
    {
        for (uint32_t i = 0; i < bar_w; i++)
        {
            fb[(bar_y + j) * w + bar_x + i] = i < progress ? accent : palette[0];
        }
    }

    char text[64];
    uint32_t len = 0;
    uint32_t band = 64 * CONSOLE_FONT_W;
    uint32_t band_x = cx - band / 2;

    while (status_shown[len] && len < 63)
    {
        text[len] = status_shown[len];
        len++;
    }

    text[len] = 0;

    for (uint32_t j = 0; j < CONSOLE_FONT_H; j++)
    {
        for (uint32_t i = 0; i < band; i++)
        {
            fb[(bar_y + 20 + j) * w + band_x + i] = splash_back(band_x + i, bar_y + 20 + j);
        }
    }

    small_text(fb, w, cx - len * CONSOLE_FONT_W / 2, bar_y + 20, text, palette[COLOR_DIM]);

    if (first)
    {
        small_text(fb, w, cx - 17 * CONSOLE_FONT_W / 2, h - 60, "Esc shows the log", palette[COLOR_DIM]);
        gpu_flush(0, 0, w, h);
    }
    else
    {
        gpu_flush(0, sy, w, bar_y + 20 + CONSOLE_FONT_H - sy);
    }

    splash_drawn_at = now ? now : 1;
    splash_drawn_lines = splash_lines;
}

static void render(void)
{
    uint32_t first = rows;
    uint32_t last = 0;

    if (redraw_all)
    {
        fill(0, 0, gpu_width(), gpu_height(), palette[COLOR_BG]);

        for (uint32_t y = 0; y < rows; y++)
        {
            dirty[y] = 1;
        }

        scroll_pending = 0;
    }
    else if (scroll_pending)
    {
        if (scroll_pending < rows)
        {
            move_pixels_up(scroll_pending);
        }
        else
        {
            for (uint32_t y = 0; y < rows; y++)
            {
                dirty[y] = 1;
            }
        }

        first = 0;
        last = rows - 1;
        drawn_cursor_y = drawn_cursor_y >= scroll_pending ? drawn_cursor_y - scroll_pending : 0;
        scroll_pending = 0;
    }

    mark(drawn_cursor_y);
    mark(cursor_y);

    for (uint32_t y = 0; y < rows; y++)
    {
        if (!dirty[y])
        {
            continue;
        }

        dirty[y] = 0;

        for (uint32_t x = 0; x < columns; x++)
        {
            draw_cell(y, x, cursor_visible && y == cursor_y && x == cursor_x);
        }

        first = y < first ? y : first;
        last = y > last ? y : last;
    }

    drawn_cursor_x = cursor_x;
    drawn_cursor_y = cursor_y;

    if (redraw_all)
    {
        redraw_all = 0;
        gpu_flush(0, 0, gpu_width(), gpu_height());
    }
    else if (first <= last)
    {
        gpu_flush(0, MARGIN_Y + first * CONSOLE_FONT_H, gpu_width(), (last - first + 1) * CONSOLE_FONT_H);
    }
}

void fbcon_reveal(void)
{
    if (splash)
    {
        splash = 0;
        redraw_all = 1;
    }
}

void fbcon_init_splash(void)
{
    splash = 1;
}

void fbcon_init(void)
{
    if (!gpu_ready())
    {
        return;
    }

    uint64_t interrupts = spin_lock(&lock);

    columns = (gpu_width() - 2 * MARGIN_X) / CONSOLE_FONT_W;
    rows = (gpu_height() - 2 * MARGIN_Y) / CONSOLE_FONT_H;
    columns = columns > MAX_COLUMNS ? MAX_COLUMNS : columns;
    rows = rows > MAX_ROWS ? MAX_ROWS : rows;
    region_top = 0;
    region_bottom = rows - 1;

    for (uint32_t y = 0; y < rows; y++)
    {
        clear_cells(y, 0, columns);
    }

    active = 1;
    redraw_all = 1;

    for (uint32_t i = 0; i < early_length; i++)
    {
        put_byte((uint8_t)early[i]);
    }

    early_length = 0;
    spin_unlock(&lock, interrupts);
}

void fbcon_putc(char c)
{
    uint64_t interrupts = spin_lock(&lock);

    if (active)
    {
        put_byte((uint8_t)c);
    }
    else if (early_length < EARLY_SIZE)
    {
        early[early_length++] = c;
    }

    spin_unlock(&lock, interrupts);
}

void fbcon_tick(void)
{
    if (!active || paused || !gpu_ready() || !spin_trylock(&lock))
    {
        return;
    }

    if (splash)
    {
        uint64_t now = timer_ticks();

        if (splash_since == 0)
        {
            splash_since = now ? now : 1;
        }

        if (now - splash_since > 3000)
        {
            splash = 0;
            redraw_all = 1;
        }
        else
        {
            if (splash_drawn_at == 0 || now - splash_drawn_at >= 6 || splash_lines != splash_drawn_lines)
            {
                render_splash();
            }

            spin_unlock(&lock, 0);
            return;
        }
    }

    int work = redraw_all || scroll_pending || drawn_cursor_x != cursor_x || drawn_cursor_y != cursor_y;

    for (uint32_t y = 0; y < rows && !work; y++)
    {
        work = dirty[y];
    }

    if (work)
    {
        render();
    }

    spin_unlock(&lock, 0);
}

void fbcon_pause(void)
{
    uint64_t interrupts = spin_lock(&lock);

    paused = 1;
    spin_unlock(&lock, interrupts);
}

void fbcon_resume(void)
{
    uint64_t interrupts = spin_lock(&lock);

    paused = 0;
    splash = 0;
    redraw_all = 1;
    spin_unlock(&lock, interrupts);
}

int fbcon_active(void)
{
    return active && !paused;
}

uint32_t fbcon_columns(void)
{
    return columns;
}

uint32_t fbcon_rows(void)
{
    return rows;
}
