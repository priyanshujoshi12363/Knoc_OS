#include "wallpaper.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

typedef struct accent
{
    const char *name;
    uint32_t dark;
    uint32_t light;
} accent_t;

static const accent_t accents[ACCENT_COUNT] = {
    {"ember", 0xF08A5D, 0xC2542A},
    {"moss", 0x9CC58A, 0x4E7F3F},
    {"sand", 0xD9BC8C, 0x8F6A2E},
    {"rose", 0xE892AE, 0xB24A6E},
    {"mono", 0xE8E9EA, 0x2A2E33},
};

static const char *const walls[WALL_COUNT] = {"ridge", "dune", "strata", "grid"};

typedef struct palette
{
    int dark;
    uint32_t accent;
    uint32_t top;
    uint32_t base;
    uint32_t far;
    uint32_t near;
    uint32_t line;
} palette_t;

typedef struct noise
{
    float values[64];
    int count;
} noise_t;

static uint32_t seed_state;

const char *accent_name(int accent)
{
    return accent >= 0 && accent < ACCENT_COUNT ? accents[accent].name : "ember";
}

uint32_t accent_color(int accent, int theme)
{
    if (accent < 0 || accent >= ACCENT_COUNT)
    {
        accent = ACCENT_EMBER;
    }

    return theme == THEME_PAPER ? accents[accent].light : accents[accent].dark;
}

const char *wallpaper_name(int wall)
{
    return wall >= 0 && wall < WALL_COUNT ? walls[wall] : "ridge";
}

int wallpaper_find(const char *name)
{
    for (int i = 0; i < WALL_COUNT; i++)
    {
        if (strcmp(name, walls[i]) == 0)
        {
            return i;
        }
    }

    return -1;
}

int accent_find(const char *name)
{
    for (int i = 0; i < ACCENT_COUNT; i++)
    {
        if (strcmp(name, accents[i].name) == 0)
        {
            return i;
        }
    }

    return -1;
}

static float random_next(void)
{
    seed_state = seed_state * 1664525u + 1013904223u;
    return (float)seed_state / 4294967296.0f;
}

static void noise_make(noise_t *n, int count)
{
    n->count = count;

    for (int i = 0; i < count; i++)
    {
        n->values[i] = random_next();
    }
}

static float noise_at(const noise_t *n, float x)
{
    int i = (int)floorf(x);
    float f = x - (float)i;
    int a = ((i % n->count) + n->count) % n->count;
    int b = (a + 1) % n->count;

    f = f * f * (3 - 2 * f);
    return n->values[a] + (n->values[b] - n->values[a]) * f;
}

static uint32_t mixf(uint32_t a, uint32_t b, float t)
{
    int v = (int)(t * 255.0f + 0.5f);

    return rgb_mix(a, b, v < 0 ? 0 : v > 255 ? 255 : v);
}

static void grain(canvas_t *c, int strength)
{
    uint32_t s = 7;

    for (int y = 0; y < c->height; y++)
    {
        uint32_t *row = c->pixels + (long)y * c->stride;

        for (int x = 0; x < c->width; x++)
        {
            s = s * 1103515245u + 12345u;
            int d = (int)((s >> 16) & 0xFF) - 128;
            int delta = d * strength / 128;
            uint32_t p = row[x];
            int r = (int)((p >> 16) & 0xFF) + delta;
            int g = (int)((p >> 8) & 0xFF) + delta;
            int b = (int)(p & 0xFF) + delta;

            r = r < 0 ? 0 : r > 255 ? 255 : r;
            g = g < 0 ? 0 : g > 255 ? 255 : g;
            b = b < 0 ? 0 : b > 255 ? 255 : b;
            row[x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
}

static void glow(canvas_t *c, float cx, float cy, float radius, uint32_t color, float strength)
{
    for (int y = 0; y < c->height; y++)
    {
        uint32_t *row = c->pixels + (long)y * c->stride;

        for (int x = 0; x < c->width; x++)
        {
            float dx = (float)x - cx;
            float dy = (float)y - cy;
            float t = 1.0f - sqrtf(dx * dx + dy * dy) / radius;

            if (t > 0)
            {
                row[x] = mixf(row[x], color, t * t * strength);
            }
        }
    }
}

static void vertical(canvas_t *c, uint32_t top, uint32_t bottom, float until)
{
    int end = (int)((float)c->height * until);

    for (int y = 0; y < c->height; y++)
    {
        float t = y >= end ? 1.0f : (float)y / (float)end;

        draw_fill(c, 0, y, c->width, 1, mixf(top, bottom, t));
    }
}

static void ridge(canvas_t *c, const palette_t *p)
{
    float w = (float)c->width;
    float h = (float)c->height;

    vertical(c, p->top, mixf(p->base, p->accent, p->dark ? 0.34f : 0.30f), 0.7f);
    glow(c, w * 0.68f, h * 0.5f, w * 0.55f, p->accent, p->dark ? 0.45f : 0.32f);
    draw_circle(c, w * 0.68f, h * 0.47f, w * 0.022f, p->dark ? 0xFFF3E8 : 0xFFFFFF, p->dark ? 217 : 230);

    seed_state = 11;

    for (int k = 0; k < 7; k++)
    {
        float t = (float)k / 6.0f;
        noise_t n1, n2, n3;

        noise_make(&n1, 64);
        noise_make(&n2, 64);
        noise_make(&n3, 64);

        float base = h * (0.50f + t * 0.36f);
        float amp = h * (0.16f - t * 0.06f);
        uint32_t color = mixf(p->far, p->near, powf(t, 0.85f));
        uint32_t deep = mixf(color, p->near, 0.35f);

        for (int x = 0; x < c->width; x++)
        {
            float u = (float)x / w * 6.0f;
            float top = base - amp * (noise_at(&n1, u) * 0.62f + noise_at(&n2, u * 2.3f) * 0.28f +
                                      noise_at(&n3, u * 6.1f) * 0.10f);
            int start = (int)floorf(top);
            float edge = top - (float)start;

            if (start >= 0 && start < c->height)
            {
                draw_pixel(c, x, start, color, (int)((1.0f - edge) * 255.0f));
            }

            for (int y = start + 1; y < c->height; y++)
            {
                float f = ((float)y - (base - amp)) / (h - (base - amp));

                c->pixels[(long)y * c->stride + x] = mixf(color, deep, f < 0 ? 0 : f > 1 ? 1 : f);
            }

        }
    }

    grain(c, 9);
}

static void dune(canvas_t *c, const palette_t *p)
{
    float w = (float)c->width;
    float h = (float)c->height;
    uint32_t from = mixf(p->top, p->accent, p->dark ? 0.10f : 0.12f);
    uint32_t to = mixf(p->base, p->accent, 0.30f);

    for (int y = 0; y < c->height; y++)
    {
        for (int x = 0; x < c->width; x++)
        {
            c->pixels[(long)y * c->stride + x] = mixf(from, to, ((float)x / w + (float)y / h) * 0.5f);
        }
    }

    seed_state = 23;

    for (int k = 0; k < 6; k++)
    {
        float t = (float)k / 5.0f;
        noise_t n, m;

        noise_make(&n, 32);
        noise_make(&m, 32);

        float base = h * (0.22f + t * 0.62f);
        float amp = h * 0.10f;
        uint32_t lit = mixf(p->base, p->accent, (p->dark ? 0.55f : 0.38f) - t * 0.12f);
        uint32_t shade = mixf(p->dark ? 0x07080A : 0x9AA0A6, p->accent, p->dark ? 0.10f : 0.18f);
        uint32_t middle = mixf(lit, shade, 0.55f);

        for (int x = 0; x < c->width; x++)
        {
            float u = (float)x / w * 2.4f + (float)k;
            float top = base + amp * (sinf(u * 1.7f + (float)k) * 0.55f + noise_at(&n, u * 1.3f) * 0.6f +
                                      noise_at(&m, u * 3.0f) * 0.12f);
            int start = (int)top;

            for (int y = start < 0 ? 0 : start; y < c->height; y++)
            {
                float f = ((float)y - (base - amp)) / (amp * 4.2f);
                uint32_t color = f < 0.45f ? mixf(lit, middle, f / 0.45f) : mixf(middle, shade, (f - 0.45f) / 0.55f);

                c->pixels[(long)y * c->stride + x] = f > 1 ? shade : color;
            }

            draw_pixel(c, x, start, 0xFFFFFF, p->dark ? 26 : 90);
        }
    }

    grain(c, 10);
}

static void strata(canvas_t *c, const palette_t *p)
{
    float w = (float)c->width;
    float h = (float)c->height;
    float bumps[9][4];

    for (int y = 0; y < c->height; y++)
    {
        for (int x = 0; x < c->width; x++)
        {
            float dx = (float)x - w * 0.3f;
            float dy = (float)y - h * 0.35f;
            float t = sqrtf(dx * dx + dy * dy) / (w * 0.9f);

            c->pixels[(long)y * c->stride + x] =
                mixf(mixf(p->base, p->accent, p->dark ? 0.16f : 0.14f), p->top, t > 1 ? 1 : t);
        }
    }

    seed_state = 5;

    for (int i = 0; i < 9; i++)
    {
        bumps[i][0] = random_next() * w;
        bumps[i][1] = random_next() * h;
        bumps[i][2] = (0.08f + random_next() * 0.22f) * w;
        bumps[i][3] = random_next() * 2 - 1;
    }

    int cell = c->width / 160 > 4 ? c->width / 160 : 4;
    int nx = c->width / cell + 2;
    int ny = c->height / cell + 2;
    float *field = malloc((size_t)nx * ny * sizeof(float));

    if (!field)
    {
        return;
    }

    for (int j = 0; j < ny; j++)
    {
        for (int i = 0; i < nx; i++)
        {
            float v = 0;

            for (int b = 0; b < 9; b++)
            {
                float dx = (float)(i * cell) - bumps[b][0];
                float dy = (float)(j * cell) - bumps[b][1];

                v += bumps[b][3] * expf(-(dx * dx + dy * dy) / (bumps[b][2] * bumps[b][2]));
            }

            field[j * nx + i] = v;
        }
    }

    for (int level = -12; level <= 12; level++)
    {
        float lv = (float)level / 10.0f + 0.001f;
        int major = level % 5 == 0;
        uint32_t color = major ? p->accent : p->line;
        int alpha = major ? 150 : p->dark ? 26 : 31;

        for (int j = 0; j < ny - 1; j++)
        {
            for (int i = 0; i < nx - 1; i++)
            {
                float a = field[j * nx + i] - lv;
                float b = field[j * nx + i + 1] - lv;
                float cc = field[(j + 1) * nx + i + 1] - lv;
                float d = field[(j + 1) * nx + i] - lv;
                float x0 = (float)(i * cell);
                float y0 = (float)(j * cell);
                float e[4][2];
                int count = 0;

                if ((a > 0) != (b > 0))
                {
                    e[count][0] = x0 + (float)cell * a / (a - b);
                    e[count++][1] = y0;
                }

                if ((b > 0) != (cc > 0))
                {
                    e[count][0] = x0 + (float)cell;
                    e[count++][1] = y0 + (float)cell * b / (b - cc);
                }

                if ((cc > 0) != (d > 0))
                {
                    e[count][0] = x0 + (float)cell * (1 - cc / (cc - d));
                    e[count++][1] = y0 + (float)cell;
                }

                if ((d > 0) != (a > 0))
                {
                    e[count][0] = x0;
                    e[count++][1] = y0 + (float)cell * (1 - d / (d - a));
                }

                if (count >= 2)
                {
                    draw_line(c, e[0][0], e[0][1], e[1][0], e[1][1], color, alpha);
                }

                if (count == 4)
                {
                    draw_line(c, e[2][0], e[2][1], e[3][0], e[3][1], color, alpha);
                }
            }
        }
    }

    free(field);
    grain(c, 7);
}

static void dots(canvas_t *c, const palette_t *p)
{
    float s = (float)c->width / 58.0f;

    draw_fill(c, 0, 0, c->width, c->height, p->base);

    for (float y = s / 2; y < (float)c->height; y += s)
    {
        for (float x = s / 2; x < (float)c->width; x += s)
        {
            draw_circle(c, x, y, 1.0f, p->dark ? 0x2A2E33 : 0xC4C9CF, 255);
        }
    }

    draw_fill(c, (int)((float)c->width - s * 3.5f), (int)((float)c->height - s * 3.5f), (int)(s * 0.5f),
              (int)(s * 0.5f), p->accent);
}

void wallpaper_draw(canvas_t *c, int wall, int theme, int accent)
{
    palette_t p;
    uint32_t a = accent_color(accent, theme);

    p.dark = theme != THEME_PAPER;
    p.accent = a;

    if (p.dark)
    {
        p.top = 0x0B0D0F;
        p.base = 0x121416;
        p.far = mixf(0x3A3F46, a, 0.52f);
        p.near = 0x08090B;
        p.line = 0xECEDEE;
    }
    else
    {
        p.top = 0xEEF0F2;
        p.base = 0xE4E6E9;
        p.far = mixf(0xD3D7DC, a, 0.30f);
        p.near = mixf(0x8E959C, a, 0.18f);
        p.line = 0x111418;
    }

    if (wall == WALL_DUNE)
    {
        dune(c, &p);
    }
    else if (wall == WALL_STRATA)
    {
        strata(c, &p);
    }
    else if (wall == WALL_GRID)
    {
        dots(c, &p);
    }
    else
    {
        ridge(c, &p);
    }
}
