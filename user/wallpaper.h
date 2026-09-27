#ifndef WALLPAPER_H
#define WALLPAPER_H

#include "draw.h"

enum
{
    THEME_GRAPHITE,
    THEME_PAPER,
};

enum
{
    ACCENT_EMBER,
    ACCENT_MOSS,
    ACCENT_SAND,
    ACCENT_ROSE,
    ACCENT_MONO,
    ACCENT_COUNT,
};

enum
{
    WALL_RIDGE,
    WALL_DUNE,
    WALL_STRATA,
    WALL_GRID,
    WALL_COUNT,
};

const char *accent_name(int accent);
uint32_t accent_color(int accent, int theme);
const char *wallpaper_name(int wall);
int wallpaper_find(const char *name);
int accent_find(const char *name);
void wallpaper_draw(canvas_t *c, int wall, int theme, int accent);

#endif
