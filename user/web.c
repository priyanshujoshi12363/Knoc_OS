#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "http.h"
#include "ulib.h"

#define WIDTH 78
#define PAGE_LINES 20
#define PAGE_MAX (4 * 1024 * 1024)
#define HISTORY_MAX 32
#define LINE_MAX 1024
#define WORD_MAX 512
#define TITLE_MAX 160
#define LIST_DEPTH 8
#define BOLD_ON '\001'
#define BOLD_OFF '\002'
#define MARK_ON '\003'
#define MARK_OFF '\004'
#define SEARCH_URL "https://lite.duckduckgo.com/lite/?q="

typedef struct page
{
    char url[HTTP_URL_MAX];
    char title[TITLE_MAX];
    int status;
    char **lines;
    int line_count;
    int line_room;
    char **links;
    int link_count;
    int link_room;
} page_t;

typedef struct loader
{
    char *data;
    size_t length;
    size_t room;
    int binary;
} loader_t;

static page_t page;
static int dump_mode;
static char *history[HISTORY_MAX];
static int history_count;

static char line[LINE_MAX];
static int line_used;
static int line_width;
static int line_content;
static int pending_break;
static int pending_space;
static int last_blank = 1;
static int in_pre;
static int block_indent;
static int list_depth;
static int list_ordered[LIST_DEPTH];
static int list_counter[LIST_DEPTH];
static char bullet[16];
static int hang;
static int open_link;
static int cell_index;
static int in_title;

static void *grow(void *array, int *room, int count, size_t size)
{
    if (count < *room)
    {
        return array;
    }

    int bigger = *room ? *room * 2 : 64;
    void *grown = realloc(array, (size_t)bigger * size);

    if (grown)
    {
        *room = bigger;
    }

    return grown;
}

static void clear_page(void)
{
    for (int i = 0; i < page.line_count; i++)
    {
        free(page.lines[i]);
    }

    for (int i = 0; i < page.link_count; i++)
    {
        free(page.links[i]);
    }

    page.line_count = 0;
    page.link_count = 0;
    page.title[0] = 0;
}

static void push_line(const char *text)
{
    char **grown = grow(page.lines, &page.line_room, page.line_count, sizeof(char *));

    if (!grown)
    {
        return;
    }

    page.lines = grown;
    page.lines[page.line_count++] = strdup(text);
    last_blank = text[0] == 0;
}

static int add_link(const char *url)
{
    char **grown = grow(page.links, &page.link_room, page.link_count, sizeof(char *));

    if (!grown)
    {
        return 0;
    }

    page.links = grown;
    page.links[page.link_count++] = strdup(url);
    return page.link_count;
}

static int display_width(const char *text, size_t length)
{
    int width = 0;

    for (size_t i = 0; i < length; i++)
    {
        unsigned char ch = (unsigned char)text[i];

        if ((ch & 0xC0) != 0x80 && ch >= ' ')
        {
            width++;
        }
    }

    return width;
}

static void add_to_line(const char *text, size_t length)
{
    if (line_used + (int)length >= LINE_MAX - 1)
    {
        length = (size_t)(LINE_MAX - 1 - line_used);
    }

    memcpy(line + line_used, text, length);
    line_used += (int)length;
    line[line_used] = 0;
    line_width += display_width(text, length);
}

static void end_line(void)
{
    while (line_used > 0 && line[line_used - 1] == ' ')
    {
        line_used--;
    }

    line[line_used] = 0;
    push_line(line);
    line_used = 0;
    line_width = 0;
    line_content = 0;
    line[0] = 0;
}

static int base_indent(void)
{
    return block_indent + list_depth * 2;
}

static void start_line(void)
{
    int spaces = base_indent() + (bullet[0] ? 0 : hang);

    if (spaces > WIDTH / 2)
    {
        spaces = WIDTH / 2;
    }

    for (int i = 0; i < spaces; i++)
    {
        add_to_line(" ", 1);
    }

    if (bullet[0])
    {
        hang = (int)strlen(bullet);
        add_to_line(bullet, strlen(bullet));
        bullet[0] = 0;
    }
}

static void apply_breaks(void)
{
    if (pending_break == 0)
    {
        return;
    }

    if (line_content)
    {
        end_line();
    }

    if (pending_break >= 2 && !last_blank)
    {
        push_line("");
    }

    pending_break = 0;
    pending_space = 0;
}

static void request_break(int count)
{
    if (count > pending_break)
    {
        pending_break = count;
    }
}

static void emit_word(const char *word, size_t length, int glue)
{
    if (length == 0 || in_title)
    {
        return;
    }

    apply_breaks();

    int width = display_width(word, length);
    int space = line_content && pending_space && !glue;

    if (line_content && line_width + space + width > WIDTH)
    {
        end_line();
        space = 0;
    }

    if (!line_content && line_used == 0)
    {
        start_line();
    }

    if (space)
    {
        add_to_line(" ", 1);
    }

    while (line_width + width > WIDTH && width > WIDTH - line_width && line_width < WIDTH)
    {
        size_t take = 0;
        int room = WIDTH - line_width;
        int counted = 0;

        while (take < length && counted < room)
        {
            take++;

            while (take < length && ((unsigned char)word[take] & 0xC0) == 0x80)
            {
                take++;
            }

            counted++;
        }

        add_to_line(word, take);
        line_content = 1;
        end_line();
        start_line();
        word += take;
        length -= take;
        width = display_width(word, length);
    }

    add_to_line(word, length);
    line_content = 1;
    pending_space = 0;
}

static void emit_marker(char on, const char *text, char off, int glue)
{
    char word[WORD_MAX];
    int n = snprintf(word, sizeof(word), "%c%s%c", on, text, off);

    emit_word(word, (size_t)n, glue);
}

static int put_utf8(char *out, unsigned long code)
{
    if (code < 0x80)
    {
        out[0] = (char)code;
        return 1;
    }

    if (code < 0x800)
    {
        out[0] = (char)(0xC0 | (code >> 6));
        out[1] = (char)(0x80 | (code & 0x3F));
        return 2;
    }

    if (code < 0x10000)
    {
        out[0] = (char)(0xE0 | (code >> 12));
        out[1] = (char)(0x80 | ((code >> 6) & 0x3F));
        out[2] = (char)(0x80 | (code & 0x3F));
        return 3;
    }

    if (code < 0x110000)
    {
        out[0] = (char)(0xF0 | (code >> 18));
        out[1] = (char)(0x80 | ((code >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((code >> 6) & 0x3F));
        out[3] = (char)(0x80 | (code & 0x3F));
        return 4;
    }

    out[0] = '?';
    return 1;
}

static const struct
{
    const char *name;
    unsigned long code;
} entities[] = {
    {"amp", '&'},       {"lt", '<'},        {"gt", '>'},        {"quot", '"'},      {"apos", '\''},
    {"nbsp", ' '},      {"ensp", ' '},      {"emsp", ' '},      {"thinsp", ' '},    {"shy", 0},
    {"copy", 0xA9},     {"reg", 0xAE},      {"trade", 0x2122},  {"mdash", 0x2014},  {"ndash", 0x2013},
    {"hellip", 0x2026}, {"laquo", 0xAB},    {"raquo", 0xBB},    {"lsquo", 0x2018},  {"rsquo", 0x2019},
    {"ldquo", 0x201C},  {"rdquo", 0x201D},  {"bull", 0x2022},   {"middot", 0xB7},   {"times", 0xD7},
    {"divide", 0xF7},   {"euro", 0x20AC},   {"pound", 0xA3},    {"yen", 0xA5},      {"cent", 0xA2},
    {"deg", 0xB0},      {"plusmn", 0xB1},   {"para", 0xB6},     {"sect", 0xA7},     {"larr", 0x2190},
    {"rarr", 0x2192},   {"uarr", 0x2191},   {"darr", 0x2193},   {"hearts", 0x2665}, {"star", 0x2606},
    {"eacute", 0xE9},   {"egrave", 0xE8},   {"aacute", 0xE1},   {"agrave", 0xE0},   {"ouml", 0xF6},
    {"uuml", 0xFC},     {"auml", 0xE4},     {"szlig", 0xDF},    {"ccedil", 0xE7},   {"ntilde", 0xF1},
    {"iacute", 0xED},   {"oacute", 0xF3},   {"uacute", 0xFA},   {"zwj", 0},         {"zwnj", 0},
};

static size_t decode(const char *text, size_t length, char *out, size_t room)
{
    size_t used = 0;

    for (size_t i = 0; i < length && used + 4 < room;)
    {
        if (text[i] != '&')
        {
            out[used++] = text[i++];
            continue;
        }

        size_t end = i + 1;

        while (end < length && end - i < 12 && text[end] != ';' && text[end] != '&' && !isspace((unsigned char)text[end]))
        {
            end++;
        }

        if (end >= length || text[end] != ';')
        {
            out[used++] = text[i++];
            continue;
        }

        const char *name = text + i + 1;
        size_t name_length = end - i - 1;
        long code = -1;

        if (name_length > 1 && name[0] == '#')
        {
            char digits[12];

            memcpy(digits, name + 1, name_length - 1);
            digits[name_length - 1] = 0;
            code = digits[0] == 'x' || digits[0] == 'X' ? strtol(digits + 1, 0, 16) : strtol(digits, 0, 10);

            if (code == 0xA0)
            {
                code = ' ';
            }
        }
        else
        {
            for (size_t e = 0; e < sizeof(entities) / sizeof(entities[0]); e++)
            {
                if (strlen(entities[e].name) == name_length && strncmp(entities[e].name, name, name_length) == 0)
                {
                    code = (long)entities[e].code;
                    break;
                }
            }
        }

        if (code < 0)
        {
            out[used++] = text[i++];
            continue;
        }

        if (code > 0)
        {
            used += (size_t)put_utf8(out + used, (unsigned long)code);
        }

        i = end + 1;
    }

    out[used] = 0;
    return used;
}

static void emit_pre(const char *text, size_t length)
{
    apply_breaks();

    for (size_t i = 0; i < length; i++)
    {
        char ch = text[i];

        if (ch == '\r')
        {
            continue;
        }

        if (!line_content && line_used == 0)
        {
            start_line();
            line_content = 1;
        }

        if (ch == '\n')
        {
            end_line();
            continue;
        }

        if (ch == '\t')
        {
            do
            {
                add_to_line(" ", 1);
            } while (line_width % 8 != 0 && line_width < WIDTH);

            continue;
        }

        if (line_width >= WIDTH && ((unsigned char)ch & 0xC0) != 0x80)
        {
            end_line();
            start_line();
            line_content = 1;
        }

        add_to_line(&ch, 1);
    }
}

static void emit_text(const char *raw, size_t raw_length)
{
    static char text[65536];
    size_t length = decode(raw, raw_length < sizeof(text) - 8 ? raw_length : sizeof(text) - 8, text, sizeof(text));

    if (in_title)
    {
        size_t used = strlen(page.title);

        for (size_t i = 0; i < length && used < TITLE_MAX - 1; i++)
        {
            char ch = isspace((unsigned char)text[i]) ? ' ' : text[i];

            if (ch != ' ' || (used > 0 && page.title[used - 1] != ' '))
            {
                page.title[used++] = ch;
            }
        }

        page.title[used] = 0;
        return;
    }

    if (in_pre)
    {
        emit_pre(text, length);
        return;
    }

    size_t i = 0;

    while (i < length)
    {
        if (isspace((unsigned char)text[i]))
        {
            pending_space = 1;
            i++;
            continue;
        }

        size_t start = i;

        while (i < length && !isspace((unsigned char)text[i]))
        {
            i++;
        }

        emit_word(text + start, i - start, 0);
    }
}

static int get_attribute(const char *tag, size_t length, const char *name, char *out, size_t room)
{
    size_t name_length = strlen(name);
    size_t i = 0;

    while (i < length && !isspace((unsigned char)tag[i]))
    {
        i++;
    }

    while (i < length)
    {
        while (i < length && (isspace((unsigned char)tag[i]) || tag[i] == '/'))
        {
            i++;
        }

        size_t key = i;

        while (i < length && tag[i] != '=' && !isspace((unsigned char)tag[i]) && tag[i] != '/')
        {
            i++;
        }

        size_t key_length = i - key;

        while (i < length && isspace((unsigned char)tag[i]))
        {
            i++;
        }

        const char *value = "";
        size_t value_length = 0;

        if (i < length && tag[i] == '=')
        {
            i++;

            while (i < length && isspace((unsigned char)tag[i]))
            {
                i++;
            }

            if (i < length && (tag[i] == '"' || tag[i] == '\''))
            {
                char quote = tag[i++];

                value = tag + i;

                while (i < length && tag[i] != quote)
                {
                    i++;
                }

                value_length = (size_t)(tag + i - value);
                i++;
            }
            else
            {
                value = tag + i;

                while (i < length && !isspace((unsigned char)tag[i]))
                {
                    i++;
                }

                value_length = (size_t)(tag + i - value);
            }
        }

        if (key_length == name_length && strncasecmp(tag + key, name, name_length) == 0)
        {
            decode(value, value_length < room / 2 ? value_length : room / 2, out, room);
            return 1;
        }

        if (key_length == 0)
        {
            i++;
        }
    }

    out[0] = 0;
    return 0;
}

static int is_tag(const char *name, const char *list)
{
    size_t length = strlen(name);

    while (*list)
    {
        size_t item = strcspn(list, " ");

        if (item == length && strncmp(list, name, length) == 0)
        {
            return 1;
        }

        list += item;

        while (*list == ' ')
        {
            list++;
        }
    }

    return 0;
}

static void close_link(void)
{
    if (open_link)
    {
        char number[16];

        snprintf(number, sizeof(number), "[%d]", open_link);
        emit_marker(MARK_ON, number, MARK_OFF, 1);
        open_link = 0;
    }
}

static void handle_tag(const char *name, int closing, const char *tag, size_t length)
{
    char value[HTTP_URL_MAX];

    if (strcmp(name, "title") == 0)
    {
        in_title = !closing && page.title[0] == 0;
        return;
    }

    if (strcmp(name, "br") == 0)
    {
        apply_breaks();

        if (line_content)
        {
            end_line();
        }
        else if (!last_blank)
        {
            push_line("");
        }

        pending_space = 0;
        return;
    }

    if (strcmp(name, "p") == 0)
    {
        request_break(2);
        return;
    }

    if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && name[2] == 0)
    {
        if (closing)
        {
            emit_marker(BOLD_OFF, "", BOLD_OFF, 1);
            request_break(2);
        }
        else
        {
            char marks[8];
            int level = name[1] - '0';

            request_break(2);
            memset(marks, '#', (size_t)level);
            marks[level] = 0;

            if (dump_mode)
            {
                emit_word(marks, (size_t)level, 0);
            }

            emit_marker(BOLD_ON, "", BOLD_ON, 1);
            pending_space = dump_mode;
        }

        return;
    }

    if (strcmp(name, "ul") == 0 || strcmp(name, "ol") == 0 || strcmp(name, "menu") == 0)
    {
        request_break(list_depth == 0 ? 2 : 1);

        if (closing)
        {
            if (list_depth > 0)
            {
                list_depth--;
            }

            hang = 0;
            bullet[0] = 0;
        }
        else if (list_depth < LIST_DEPTH)
        {
            list_ordered[list_depth] = name[0] == 'o';
            list_counter[list_depth] = 0;
            list_depth++;
            hang = 0;
        }

        return;
    }

    if (strcmp(name, "li") == 0)
    {
        request_break(1);
        hang = 0;

        if (!closing)
        {
            int level = list_depth > 0 ? list_depth - 1 : 0;

            if (list_depth > 0 && list_ordered[level])
            {
                snprintf(bullet, sizeof(bullet), "%d. ", ++list_counter[level]);
            }
            else
            {
                snprintf(bullet, sizeof(bullet), "%s ", level % 2 ? "-" : "*");
            }
        }

        return;
    }

    if (strcmp(name, "blockquote") == 0 || strcmp(name, "dd") == 0)
    {
        request_break(strcmp(name, "dd") == 0 ? 1 : 2);
        block_indent += closing ? (block_indent >= 4 ? -4 : 0) : 4;
        return;
    }

    if (strcmp(name, "pre") == 0)
    {
        request_break(2);
        in_pre = !closing;
        return;
    }

    if (strcmp(name, "hr") == 0)
    {
        request_break(1);
        apply_breaks();
        push_line("--------------------------------------------------------------------------");
        return;
    }

    if (strcmp(name, "tr") == 0)
    {
        request_break(1);
        cell_index = 0;
        return;
    }

    if ((strcmp(name, "td") == 0 || strcmp(name, "th") == 0) && !closing)
    {
        if (cell_index++ > 0)
        {
            pending_space = 1;
            emit_word("|", 1, 0);
            pending_space = 1;
        }

        return;
    }

    if (strcmp(name, "a") == 0)
    {
        close_link();

        if (!closing && get_attribute(tag, length, "href", value, sizeof(value)) && value[0] != '#' &&
            strncasecmp(value, "javascript:", 11) != 0)
        {
            char absolute[HTTP_URL_MAX];

            if (http_join(page.url, value, absolute, sizeof(absolute)) == 0)
            {
                open_link = add_link(absolute);
            }
        }

        return;
    }

    if (strcmp(name, "img") == 0)
    {
        if (get_attribute(tag, length, "alt", value, sizeof(value)) && value[0])
        {
            char text[TITLE_MAX];

            snprintf(text, sizeof(text), "[%s]", value);
            emit_text(text, strlen(text));
        }

        return;
    }

    if (strcmp(name, "input") == 0 && !closing)
    {
        char type[32];

        get_attribute(tag, length, "type", type, sizeof(type));

        if ((strcasecmp(type, "submit") == 0 || strcasecmp(type, "button") == 0) &&
            get_attribute(tag, length, "value", value, sizeof(value)) && value[0])
        {
            char text[TITLE_MAX];

            snprintf(text, sizeof(text), "[%s]", value);
            pending_space = 1;
            emit_text(text, strlen(text));
        }

        return;
    }

    if (is_tag(name, "div section article header footer nav main aside form table figure figcaption dl dt address "
                     "center fieldset details summary caption tbody thead tfoot html body"))
    {
        request_break(1);
        return;
    }

    if (is_tag(name, "td th button label option"))
    {
        pending_space = 1;
    }
}

static const char *find_close(const char *p, const char *end, const char *name)
{
    size_t length = strlen(name);

    for (; p + length + 2 <= end; p++)
    {
        if (p[0] == '<' && p[1] == '/' && strncasecmp(p + 2, name, length) == 0)
        {
            return p;
        }
    }

    return end;
}

static void render_html(const char *html, size_t size)
{
    const char *p = html;
    const char *end = html + size;

    while (p < end)
    {
        if (*p != '<')
        {
            const char *next = memchr(p, '<', (size_t)(end - p));

            if (!next)
            {
                next = end;
            }

            emit_text(p, (size_t)(next - p));
            p = next;
            continue;
        }

        if (end - p >= 4 && strncmp(p, "<!--", 4) == 0)
        {
            const char *close = p + 4;

            while (close + 3 <= end && strncmp(close, "-->", 3) != 0)
            {
                close++;
            }

            p = close + 3 <= end ? close + 3 : end;
            continue;
        }

        const char *q = p + 1;
        int closing = 0;

        if (q < end && *q == '/')
        {
            closing = 1;
            q++;
        }

        if (q < end && (*q == '!' || *q == '?'))
        {
            const char *close = memchr(q, '>', (size_t)(end - q));

            p = close ? close + 1 : end;
            continue;
        }

        char name[16];
        size_t n = 0;

        while (q < end && (isalnum((unsigned char)*q) || *q == '-') && n < sizeof(name) - 1)
        {
            name[n++] = (char)tolower((unsigned char)*q++);
        }

        name[n] = 0;

        if (n == 0)
        {
            emit_text("<", 1);
            p++;
            continue;
        }

        const char *tag_start = q;
        char quote = 0;

        while (q < end && (quote || *q != '>'))
        {
            if (quote && *q == quote)
            {
                quote = 0;
            }
            else if (!quote && (*q == '"' || *q == '\''))
            {
                quote = *q;
            }

            q++;
        }

        const char *tag_end = q;

        p = q < end ? q + 1 : end;

        if (!closing && is_tag(name, "script style template svg select textarea iframe object math"))
        {
            const char *close = find_close(p, end, name);
            const char *after = memchr(close, '>', (size_t)(end - close));

            p = after ? after + 1 : end;
            continue;
        }

        handle_tag(name, closing, tag_start - 1, (size_t)(tag_end - tag_start + 1));

        if (strcmp(name, "pre") == 0 && !closing && p < end && *p == '\n')
        {
            p++;
        }
    }

    close_link();
}

static void render_plain(const char *text, size_t size)
{
    in_pre = 1;
    emit_pre(text, size);
    in_pre = 0;
}

static void finish_render(void)
{
    if (line_content)
    {
        end_line();
    }

    while (page.line_count > 0 && page.lines[page.line_count - 1][0] == 0)
    {
        free(page.lines[--page.line_count]);
    }
}

static void reset_renderer(void)
{
    line_used = 0;
    line_width = 0;
    line_content = 0;
    pending_break = 0;
    pending_space = 0;
    last_blank = 1;
    in_pre = 0;
    block_indent = 0;
    list_depth = 0;
    bullet[0] = 0;
    hang = 0;
    open_link = 0;
    cell_index = 0;
    in_title = 0;
}

static void latin1_to_utf8(loader_t *l)
{
    size_t extra = 0;

    for (size_t i = 0; i < l->length; i++)
    {
        extra += (unsigned char)l->data[i] >= 0x80;
    }

    if (extra == 0)
    {
        return;
    }

    char *converted = malloc(l->length + extra + 1);

    if (!converted)
    {
        return;
    }

    size_t used = 0;

    for (size_t i = 0; i < l->length; i++)
    {
        unsigned char ch = (unsigned char)l->data[i];

        used += (size_t)put_utf8(converted + used, ch >= 0x80 && ch < 0xA0 ? '?' : ch);
    }

    free(l->data);
    l->data = converted;
    l->length = used;
}

static int is_text_type(const char *type)
{
    return type[0] == 0 || strncasecmp(type, "text/", 5) == 0 || strstr(type, "html") || strstr(type, "xml") ||
           strstr(type, "json") || strstr(type, "javascript");
}

static int collect(void *context, const char *data, size_t length, http_response_t *response)
{
    loader_t *l = context;

    if (!is_text_type(response->content_type))
    {
        l->binary = 1;
        snprintf(response->error, sizeof(response->error), "this is a %s file, not a web page; save it with: fetch %s",
                 response->content_type, response->url);
        return 1;
    }

    if (l->length + length > PAGE_MAX)
    {
        snprintf(response->error, sizeof(response->error), "the page is bigger than %d MiB", PAGE_MAX / 1024 / 1024);
        return 1;
    }

    if (l->length + length + 1 > l->room)
    {
        size_t room = l->room ? l->room : 65536;

        while (room < l->length + length + 1)
        {
            room *= 2;
        }

        char *grown = realloc(l->data, room);

        if (!grown)
        {
            snprintf(response->error, sizeof(response->error), "out of memory");
            return 1;
        }

        l->data = grown;
        l->room = room;
    }

    memcpy(l->data + l->length, data, length);
    l->length += length;
    return 0;
}

static int load(const char *url)
{
    loader_t l;
    http_response_t response;

    memset(&l, 0, sizeof(l));

    if (!dump_mode)
    {
        printf("web: loading %s ...\n", url);
    }

    if (http_get(url, collect, &l, &response) != 0)
    {
        printf("web: %s\n", response.error);
        free(l.data);
        return -1;
    }

    clear_page();
    reset_renderer();
    snprintf(page.url, sizeof(page.url), "%s", response.url);
    page.status = response.status;

    if (strcasestr(response.content_type, "8859-1") || strcasestr(response.content_type, "1252"))
    {
        latin1_to_utf8(&l);
    }

    const char *type = response.content_type;
    int html = type[0] == 0 || strcasestr(type, "html") || strcasestr(type, "xml");

    if (l.length > 0)
    {
        if (html)
        {
            render_html(l.data, l.length);
        }
        else
        {
            render_plain(l.data, l.length);
        }
    }

    finish_render();
    free(l.data);
    return 0;
}

static void print_line(const char *text)
{
    char out[LINE_MAX * 2];
    size_t used = 0;

    for (const char *p = text; *p && used < sizeof(out) - 8; p++)
    {
        const char *code = 0;

        if (*p == BOLD_ON)
        {
            code = "\033[1m";
        }
        else if (*p == BOLD_OFF || *p == MARK_OFF)
        {
            code = "\033[0m";
        }
        else if (*p == MARK_ON)
        {
            code = "\033[36m";
        }
        else
        {
            out[used++] = *p;
            continue;
        }

        if (!dump_mode)
        {
            size_t length = strlen(code);

            memcpy(out + used, code, length);
            used += length;
        }
    }

    out[used] = 0;
    printf("%s\n", out);
}

static void print_header(void)
{
    printf("\n");

    if (!dump_mode)
    {
        printf("\033[1m");
    }

    printf("%s", page.title[0] ? page.title : page.url);

    if (!dump_mode)
    {
        printf("\033[0m");
    }

    printf("\n%s", page.url);

    if (page.status != 200)
    {
        printf("  (the server answered %d)", page.status);
    }

    printf("\n\n");
}

static void print_links(void)
{
    if (page.link_count == 0)
    {
        printf("(this page has no links)\n");
        return;
    }

    for (int i = 0; i < page.link_count; i++)
    {
        printf("[%d] %s\n", i + 1, page.links[i]);
    }
}

static void show(int from, int count)
{
    for (int i = from; i < from + count && i < page.line_count; i++)
    {
        print_line(page.lines[i]);
    }
}

static int save_chunk(void *context, const char *data, size_t length, http_response_t *response)
{
    FILE *out = context;

    if (response->status == 200 && fwrite(data, 1, length, out) != length)
    {
        snprintf(response->error, sizeof(response->error), "writing the file failed (disk full?)");
        return 1;
    }

    return 0;
}

static void download(const char *url)
{
    http_url_t parsed;
    char name[FILE_NAME_MAX] = "index.html";
    http_response_t response;

    if (http_parse_url(url, &parsed) == 0)
    {
        parsed.path[strcspn(parsed.path, "?")] = 0;

        const char *slash = strrchr(parsed.path, '/');

        if (slash && slash[1])
        {
            snprintf(name, sizeof(name), "%s", slash + 1);
        }
    }

    FILE *out = fopen(name, "w");

    if (!out)
    {
        printf("web: cannot write %s\n", name);
        return;
    }

    printf("web: downloading %s ...\n", url);

    int result = http_get(url, save_chunk, out, &response);

    fclose(out);

    if (result != 0 || response.status != 200)
    {
        remove(name);

        if (result != 0)
        {
            printf("web: %s\n", response.error);
        }
        else
        {
            printf("web: the server answered %d\n", response.status);
        }

        return;
    }

    printf("web: saved %ld bytes to %s\n", response.received, name);
}

static void print_help(void)
{
    printf("  Enter    next screen            b        previous screen      t   top\n"
           "  NUMBER   open link NUMBER       l        list the links\n"
           "  g URL    go to a web address    s WORDS  search the web       u   go back\n"
           "  /TEXT    find text on the page  n        find the next match  r   reload\n"
           "  d NUMBER download link NUMBER into this folder                 q   quit\n");
}

static void to_url(const char *input, char *out, size_t room)
{
    if (strstr(input, "://"))
    {
        snprintf(out, room, "%s", input);
        return;
    }

    size_t host = strcspn(input, "/?#");
    int has_port = memchr(input, ':', host) != 0;
    int is_ip = host > 0 && isdigit((unsigned char)input[0]) && strspn(input, "0123456789.:") >= host;

    snprintf(out, room, "%s://%s", has_port || is_ip ? "http" : "https", input);
}

static void search_url(const char *words, char *out, size_t room)
{
    size_t used = (size_t)snprintf(out, room, "%s", SEARCH_URL);

    for (const char *p = words; *p && used + 4 < room; p++)
    {
        unsigned char ch = (unsigned char)*p;

        if (isalnum(ch) || ch == '-' || ch == '.' || ch == '_')
        {
            out[used++] = (char)ch;
        }
        else if (ch == ' ')
        {
            out[used++] = '+';
        }
        else
        {
            used += (size_t)snprintf(out + used, room - used, "%%%02X", ch);
        }
    }

    out[used] = 0;
}

static void remember(void)
{
    if (!page.url[0])
    {
        return;
    }

    if (history_count == HISTORY_MAX)
    {
        free(history[0]);
        memmove(history, history + 1, sizeof(history[0]) * (HISTORY_MAX - 1));
        history_count--;
    }

    history[history_count++] = strdup(page.url);
}

static int find_text(const char *needle, int from)
{
    for (int i = from; i < page.line_count; i++)
    {
        if (strcasestr(page.lines[i], needle))
        {
            return i;
        }
    }

    return -1;
}

static int go(const char *url, int *top)
{
    char previous[HTTP_URL_MAX];

    snprintf(previous, sizeof(previous), "%s", page.url);

    if (load(url) != 0)
    {
        return -1;
    }

    if (previous[0])
    {
        char saved[HTTP_URL_MAX];

        snprintf(saved, sizeof(saved), "%s", page.url);
        snprintf(page.url, sizeof(page.url), "%s", previous);
        remember();
        snprintf(page.url, sizeof(page.url), "%s", saved);
    }

    *top = 0;
    print_header();
    show(0, PAGE_LINES);
    return 0;
}

static void interactive(void)
{
    char input[HTTP_URL_MAX];
    char url[HTTP_URL_MAX * 2];
    char search[TITLE_MAX] = "";
    int top = 0;
    int search_line = -1;

    print_header();
    show(0, PAGE_LINES);

    for (;;)
    {
        int shown_to = top + PAGE_LINES < page.line_count ? top + PAGE_LINES : page.line_count;

        printf("-- lines %d-%d of %d, %d links -- Enter more, NUMBER open link, h help, q quit\nweb> ",
               page.line_count ? top + 1 : 0, shown_to, page.line_count, page.link_count);
        fflush(stdout);

        if (!fgets(input, sizeof(input), stdin))
        {
            return;
        }

        input[strcspn(input, "\r\n")] = 0;

        char *arg = input + 1;

        while (*arg == ' ')
        {
            arg++;
        }

        if (input[0] == 0 || strcmp(input, " ") == 0)
        {
            if (top + PAGE_LINES >= page.line_count)
            {
                printf("(end of the page)\n");
                continue;
            }

            top += PAGE_LINES;
            show(top, PAGE_LINES);
        }
        else if (isdigit((unsigned char)input[0]))
        {
            int number = atoi(input);

            if (number < 1 || number > page.link_count)
            {
                printf("web: there is no link %d on this page\n", number);
                continue;
            }

            snprintf(url, sizeof(url), "%s", page.links[number - 1]);
            go(url, &top);
        }
        else if (strcmp(input, "q") == 0)
        {
            return;
        }
        else if (strcmp(input, "b") == 0)
        {
            top = top >= PAGE_LINES ? top - PAGE_LINES : 0;
            show(top, PAGE_LINES);
        }
        else if (strcmp(input, "t") == 0)
        {
            top = 0;
            print_header();
            show(top, PAGE_LINES);
        }
        else if (strcmp(input, "l") == 0)
        {
            print_links();
        }
        else if (strcmp(input, "h") == 0 || strcmp(input, "?") == 0)
        {
            print_help();
        }
        else if (strcmp(input, "r") == 0)
        {
            snprintf(url, sizeof(url), "%s", page.url);

            if (load(url) == 0)
            {
                top = 0;
                print_header();
                show(top, PAGE_LINES);
            }
        }
        else if (strcmp(input, "u") == 0)
        {
            if (history_count == 0)
            {
                printf("web: this is the first page\n");
                continue;
            }

            char *back = history[--history_count];

            if (load(back) == 0)
            {
                top = 0;
                print_header();
                show(top, PAGE_LINES);
            }

            free(back);
        }
        else if (input[0] == 'g' && input[1] == ' ' && *arg)
        {
            to_url(arg, url, sizeof(url));
            go(url, &top);
        }
        else if (input[0] == 's' && input[1] == ' ' && *arg)
        {
            search_url(arg, url, sizeof(url));
            go(url, &top);
        }
        else if (input[0] == 'd' && input[1] == ' ' && *arg)
        {
            int number = atoi(arg);

            if (number < 1 || number > page.link_count)
            {
                printf("web: there is no link %d on this page\n", number);
                continue;
            }

            download(page.links[number - 1]);
        }
        else if (input[0] == '/' || strcmp(input, "n") == 0)
        {
            if (input[0] == '/')
            {
                snprintf(search, sizeof(search), "%s", input + 1);
                search_line = top - 1;
            }

            if (!search[0])
            {
                printf("web: type /TEXT first\n");
                continue;
            }

            int found = find_text(search, search_line + 1);

            if (found < 0)
            {
                printf("web: \"%s\" not found further down\n", search);
                continue;
            }

            search_line = found;
            top = found;
            show(top, PAGE_LINES);
        }
        else
        {
            printf("web: unknown command, type h for help\n");
        }
    }
}

int main(int argc, char **argv)
{
    char url[HTTP_URL_MAX * 2];
    int arg = 1;

    if (arg < argc && strcmp(argv[arg], "-dump") == 0)
    {
        dump_mode = 1;
        arg++;
    }

    if (arg >= argc)
    {
        printf("usage: web URL           read a web page (http:// or https://)\n"
               "       web -s WORDS      search the web\n"
               "       web -dump URL     print the page as text and exit\n");
        return 1;
    }

    if (strcmp(argv[arg], "-s") == 0 && arg + 1 < argc)
    {
        char words[TITLE_MAX] = "";

        for (int i = arg + 1; i < argc; i++)
        {
            snprintf(words + strlen(words), sizeof(words) - strlen(words), "%s%s", i > arg + 1 ? " " : "", argv[i]);
        }

        search_url(words, url, sizeof(url));
    }
    else
    {
        to_url(argv[arg], url, sizeof(url));
    }

    if (load(url) != 0)
    {
        return 1;
    }

    if (dump_mode)
    {
        print_header();
        show(0, page.line_count);
        printf("\nLinks:\n");
        print_links();
        return 0;
    }

    interactive();
    return 0;
}
