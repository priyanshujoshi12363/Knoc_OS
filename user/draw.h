#ifndef DRAW_H
#define DRAW_H

#include <stdint.h>

typedef struct canvas
{
    uint32_t *pixels;
    int width;
    int height;
    int stride;
} canvas_t;

typedef struct image
{
    uint32_t *pixels;
    int width;
    int height;
} image_t;

typedef struct font font_t;

int screen_open(canvas_t *screen);
int screen_update(const canvas_t *screen, int x, int y, int width, int height);
int canvas_create(canvas_t *canvas, int width, int height);
void canvas_free(canvas_t *canvas);

uint32_t rgb_mix(uint32_t a, uint32_t b, int t);
void draw_fill(canvas_t *c, int x, int y, int width, int height, uint32_t color);
void draw_blend(canvas_t *c, int x, int y, int width, int height, uint32_t color, int alpha);
void draw_pixel(canvas_t *c, int x, int y, uint32_t color, int alpha);
void draw_round(canvas_t *c, int x, int y, int width, int height, int radius, uint32_t color, int alpha);
void draw_frame(canvas_t *c, int x, int y, int width, int height, int radius, uint32_t color, int alpha);
void draw_gradient(canvas_t *c, int x, int y, int width, int height, uint32_t top, uint32_t bottom);
void draw_line(canvas_t *c, float x0, float y0, float x1, float y1, uint32_t color, int alpha);
void draw_circle(canvas_t *c, float cx, float cy, float radius, uint32_t color, int alpha);
void draw_canvas(canvas_t *c, const canvas_t *source, int x, int y);

int image_load(image_t *image, const char *path);
void image_free(image_t *image);
void draw_image(canvas_t *c, const image_t *image, int x, int y);

font_t *font_load(const char *path);
int draw_text(canvas_t *c, font_t *font, float size, int x, int baseline, const char *text, uint32_t color);
int text_width(font_t *font, float size, const char *text);
int font_ascent(font_t *font, float size);

#endif
