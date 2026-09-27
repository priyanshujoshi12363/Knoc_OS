#include "draw.h"
#include "ulib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../third_party/stb/stb_truetype.h"
#include "../third_party/stb/stb_image.h"

#define GLYPH_CACHE 512

typedef struct glyph
{
    uint32_t code;
    uint16_t size;
    int16_t left;
    int16_t top;
    int16_t width;
    int16_t height;
    int16_t advance;
    uint8_t *alpha;
} glyph_t;

struct font
{
    stbtt_fontinfo info;
    unsigned char *data;
    glyph_t cache[GLYPH_CACHE];
};

int screen_open(canvas_t *screen)
{
    screen_info_t info;

    if (screen_info(&info) != 0)
    {
        return -1;
    }

    uint32_t *pixels = screen_map();

    if (!pixels)
    {
        return -1;
    }

    screen->pixels = pixels;
    screen->width = (int)info.width;
    screen->height = (int)info.height;
    screen->stride = (int)(info.stride / 4);
    return 0;
}

int screen_update(const canvas_t *screen, int x, int y, int width, int height)
{
    (void)screen;
    return screen_flush((unsigned)x, (unsigned)y, (unsigned)width, (unsigned)height);
}

int canvas_create(canvas_t *canvas, int width, int height)
{
    canvas->pixels = malloc((size_t)width * height * 4);
    canvas->width = width;
    canvas->height = height;
    canvas->stride = width;
    return canvas->pixels ? 0 : -1;
}

void canvas_free(canvas_t *canvas)
{
    free(canvas->pixels);
    canvas->pixels = 0;
}

uint32_t rgb_mix(uint32_t a, uint32_t b, int t)
{
    uint32_t u = (uint32_t)t + ((uint32_t)t >> 7);
    uint32_t v = 256 - u;
    uint32_t rb = ((a & 0xFF00FF) * v + (b & 0xFF00FF) * u) >> 8;
    uint32_t g = ((a & 0x00FF00) * v + (b & 0x00FF00) * u) >> 8;

    return (rb & 0xFF00FF) | (g & 0x00FF00);
}

static int clip(const canvas_t *c, int *x, int *y, int *width, int *height)
{
    if (*x < 0)
    {
        *width += *x;
        *x = 0;
    }

    if (*y < 0)
    {
        *height += *y;
        *y = 0;
    }

    if (*x + *width > c->width)
    {
        *width = c->width - *x;
    }

    if (*y + *height > c->height)
    {
        *height = c->height - *y;
    }

    return *width > 0 && *height > 0;
}

void draw_fill(canvas_t *c, int x, int y, int width, int height, uint32_t color)
{
    if (!clip(c, &x, &y, &width, &height))
    {
        return;
    }

    for (int j = y; j < y + height; j++)
    {
        uint32_t *row = c->pixels + (long)j * c->stride;

        for (int i = x; i < x + width; i++)
        {
            row[i] = color;
        }
    }
}

void draw_blend(canvas_t *c, int x, int y, int width, int height, uint32_t color, int alpha)
{
    if (alpha >= 255)
    {
        draw_fill(c, x, y, width, height, color);
        return;
    }

    if (alpha <= 0 || !clip(c, &x, &y, &width, &height))
    {
        return;
    }

    for (int j = y; j < y + height; j++)
    {
        uint32_t *row = c->pixels + (long)j * c->stride;

        for (int i = x; i < x + width; i++)
        {
            row[i] = rgb_mix(row[i], color, alpha);
        }
    }
}

void draw_pixel(canvas_t *c, int x, int y, uint32_t color, int alpha)
{
    if (x < 0 || y < 0 || x >= c->width || y >= c->height || alpha <= 0)
    {
        return;
    }

    uint32_t *p = c->pixels + (long)y * c->stride + x;

    *p = alpha >= 255 ? color : rgb_mix(*p, color, alpha);
}

static float corner_coverage(float px, float py, float cx, float cy, float radius)
{
    float dx = px - cx;
    float dy = py - cy;
    float d = sqrtf(dx * dx + dy * dy) - radius;

    return d <= -0.5f ? 1.0f : d >= 0.5f ? 0.0f : 0.5f - d;
}

static float round_coverage(int i, int j, int x, int y, int width, int height, int radius)
{
    float px = (float)i + 0.5f;
    float py = (float)j + 0.5f;
    float r = (float)radius;
    float left = (float)x + r;
    float right = (float)(x + width) - r;
    float top = (float)y + r;
    float bottom = (float)(y + height) - r;

    if (px < left && py < top)
    {
        return corner_coverage(px, py, left, top, r);
    }

    if (px > right && py < top)
    {
        return corner_coverage(px, py, right, top, r);
    }

    if (px < left && py > bottom)
    {
        return corner_coverage(px, py, left, bottom, r);
    }

    if (px > right && py > bottom)
    {
        return corner_coverage(px, py, right, bottom, r);
    }

    return 1.0f;
}

void draw_round(canvas_t *c, int x, int y, int width, int height, int radius, uint32_t color, int alpha)
{
    if (radius * 2 > width)
    {
        radius = width / 2;
    }

    if (radius * 2 > height)
    {
        radius = height / 2;
    }

    for (int j = y; j < y + height; j++)
    {
        int corner_row = j < y + radius || j >= y + height - radius;

        if (!corner_row)
        {
            draw_blend(c, x, j, width, 1, color, alpha);
            continue;
        }

        for (int i = x; i < x + width; i++)
        {
            float cover = round_coverage(i, j, x, y, width, height, radius);

            draw_pixel(c, i, j, color, (int)(cover * (float)alpha + 0.5f));
        }
    }
}

void draw_frame(canvas_t *c, int x, int y, int width, int height, int radius, uint32_t color, int alpha)
{
    int band = radius + 2;

    for (int j = y; j < y + height; j++)
    {
        if (j < 0 || j >= c->height)
        {
            continue;
        }

        int full = j < y + band || j >= y + height - band;

        for (int i = x; i < x + width; i++)
        {
            if (!full && i == x + band && x + width - band > i)
            {
                i = x + width - band;
            }

            int edge = i == x || j == y || i == x + width - 1 || j == y + height - 1;
            float outer = round_coverage(i, j, x, y, width, height, radius);
            float inner = edge ? 0.0f : round_coverage(i, j, x + 1, y + 1, width - 2, height - 2, radius > 1 ? radius - 1 : 0);
            float cover = outer - inner;

            if (cover > 0.0f)
            {
                draw_pixel(c, i, j, color, (int)(cover * (float)alpha + 0.5f));
            }
        }
    }
}

void draw_gradient(canvas_t *c, int x, int y, int width, int height, uint32_t top, uint32_t bottom)
{
    for (int j = 0; j < height; j++)
    {
        draw_fill(c, x, y + j, width, 1, rgb_mix(top, bottom, height > 1 ? j * 255 / (height - 1) : 0));
    }
}

void draw_line(canvas_t *c, float x0, float y0, float x1, float y1, uint32_t color, int alpha)
{
    float dx = x1 - x0;
    float dy = y1 - y0;
    float length = sqrtf(dx * dx + dy * dy);
    int steps = (int)(length * 2.0f) + 1;

    for (int s = 0; s <= steps; s++)
    {
        float t = (float)s / (float)steps;
        float px = x0 + dx * t;
        float py = y0 + dy * t;
        int ix = (int)floorf(px);
        int iy = (int)floorf(py);
        float fx = px - (float)ix;
        float fy = py - (float)iy;

        draw_pixel(c, ix, iy, color, (int)((1 - fx) * (1 - fy) * (float)alpha * 0.6f));
        draw_pixel(c, ix + 1, iy, color, (int)(fx * (1 - fy) * (float)alpha * 0.6f));
        draw_pixel(c, ix, iy + 1, color, (int)((1 - fx) * fy * (float)alpha * 0.6f));
        draw_pixel(c, ix + 1, iy + 1, color, (int)(fx * fy * (float)alpha * 0.6f));
    }
}

void draw_circle(canvas_t *c, float cx, float cy, float radius, uint32_t color, int alpha)
{
    int x0 = (int)floorf(cx - radius - 1);
    int x1 = (int)ceilf(cx + radius + 1);
    int y0 = (int)floorf(cy - radius - 1);
    int y1 = (int)ceilf(cy + radius + 1);

    for (int j = y0; j <= y1; j++)
    {
        for (int i = x0; i <= x1; i++)
        {
            float cover = corner_coverage((float)i + 0.5f, (float)j + 0.5f, cx, cy, radius);

            if (cover > 0.0f)
            {
                draw_pixel(c, i, j, color, (int)(cover * (float)alpha + 0.5f));
            }
        }
    }
}

void draw_canvas(canvas_t *c, const canvas_t *source, int x, int y)
{
    for (int j = 0; j < source->height; j++)
    {
        int ty = y + j;

        if (ty < 0 || ty >= c->height)
        {
            continue;
        }

        for (int i = 0; i < source->width; i++)
        {
            int tx = x + i;

            if (tx >= 0 && tx < c->width)
            {
                c->pixels[(long)ty * c->stride + tx] = source->pixels[(long)j * source->stride + i];
            }
        }
    }
}

static unsigned char *read_file(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");

    if (!f)
    {
        return 0;
    }

    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    fseek(f, 0, SEEK_SET);

    unsigned char *data = length > 0 ? malloc((size_t)length) : 0;

    if (data && fread(data, 1, (size_t)length, f) != (size_t)length)
    {
        free(data);
        data = 0;
    }

    fclose(f);
    *size = length;
    return data;
}

int image_load(image_t *image, const char *path)
{
    long size;
    unsigned char *data = read_file(path, &size);

    if (!data)
    {
        return -1;
    }

    int w, h, channels;
    unsigned char *rgba = stbi_load_from_memory(data, (int)size, &w, &h, &channels, 4);

    free(data);

    if (!rgba)
    {
        return -1;
    }

    image->pixels = malloc((size_t)w * h * 4);

    if (!image->pixels)
    {
        stbi_image_free(rgba);
        return -1;
    }

    for (long i = 0; i < (long)w * h; i++)
    {
        unsigned char *p = rgba + i * 4;

        image->pixels[i] = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    }

    stbi_image_free(rgba);
    image->width = w;
    image->height = h;
    return 0;
}

void image_free(image_t *image)
{
    free(image->pixels);
    image->pixels = 0;
}

void draw_image(canvas_t *c, const image_t *image, int x, int y)
{
    for (int j = 0; j < image->height; j++)
    {
        for (int i = 0; i < image->width; i++)
        {
            uint32_t p = image->pixels[(long)j * image->width + i];

            draw_pixel(c, x + i, y + j, p & 0xFFFFFF, (int)(p >> 24));
        }
    }
}

font_t *font_load(const char *path)
{
    long size;
    unsigned char *data = read_file(path, &size);

    if (!data)
    {
        return 0;
    }

    font_t *font = calloc(1, sizeof(font_t));

    if (!font || !stbtt_InitFont(&font->info, data, stbtt_GetFontOffsetForIndex(data, 0)))
    {
        free(font);
        free(data);
        return 0;
    }

    font->data = data;
    return font;
}

static glyph_t *glyph_for(font_t *font, uint32_t code, float size)
{
    uint16_t key = (uint16_t)(size * 4.0f);
    uint32_t slot = (code * 31u + key * 7u) % GLYPH_CACHE;

    for (uint32_t probe = 0; probe < GLYPH_CACHE; probe++)
    {
        glyph_t *g = &font->cache[(slot + probe) % GLYPH_CACHE];

        if (g->alpha && g->code == code && g->size == key)
        {
            return g;
        }

        if (!g->alpha)
        {
            float scale = stbtt_ScaleForMappingEmToPixels(&font->info, size);
            int advance, bearing, x0, y0, x1, y1;

            stbtt_GetCodepointHMetrics(&font->info, (int)code, &advance, &bearing);
            stbtt_GetCodepointBitmapBox(&font->info, (int)code, scale, scale, &x0, &y0, &x1, &y1);

            int w = x1 - x0;
            int h = y1 - y0;

            g->alpha = calloc(1, (size_t)(w > 0 ? w : 1) * (h > 0 ? h : 1));

            if (!g->alpha)
            {
                return 0;
            }

            if (w > 0 && h > 0)
            {
                stbtt_MakeCodepointBitmap(&font->info, g->alpha, w, h, w, scale, scale, (int)code);
            }

            g->code = code;
            g->size = key;
            g->left = (int16_t)x0;
            g->top = (int16_t)y0;
            g->width = (int16_t)(w > 0 ? w : 0);
            g->height = (int16_t)(h > 0 ? h : 0);
            g->advance = (int16_t)lroundf((float)advance * scale);
            return g;
        }
    }

    return 0;
}

static uint32_t next_code(const char **text)
{
    const unsigned char *s = (const unsigned char *)*text;
    uint32_t code = *s++;
    int more = code >= 0xF0 ? 3 : code >= 0xE0 ? 2 : code >= 0xC0 ? 1 : 0;

    code &= more == 3 ? 0x07 : more == 2 ? 0x0F : more == 1 ? 0x1F : 0xFF;

    while (more-- > 0 && (*s & 0xC0) == 0x80)
    {
        code = (code << 6) | (*s++ & 0x3F);
    }

    *text = (const char *)s;
    return code;
}

int draw_text(canvas_t *c, font_t *font, float size, int x, int baseline, const char *text, uint32_t color)
{
    int pen = x;

    while (*text)
    {
        uint32_t code = next_code(&text);
        glyph_t *g = glyph_for(font, code, size);

        if (!g)
        {
            continue;
        }

        for (int j = 0; j < g->height; j++)
        {
            for (int i = 0; i < g->width; i++)
            {
                int a = g->alpha[j * g->width + i];

                if (a)
                {
                    draw_pixel(c, pen + g->left + i, baseline + g->top + j, color, a);
                }
            }
        }

        pen += g->advance;
    }

    return pen - x;
}

int text_width(font_t *font, float size, const char *text)
{
    int width = 0;

    while (*text)
    {
        glyph_t *g = glyph_for(font, next_code(&text), size);

        width += g ? g->advance : 0;
    }

    return width;
}

int font_ascent(font_t *font, float size)
{
    int ascent, descent, gap;

    stbtt_GetFontVMetrics(&font->info, &ascent, &descent, &gap);
    return (int)lroundf((float)ascent * stbtt_ScaleForMappingEmToPixels(&font->info, size));
}
