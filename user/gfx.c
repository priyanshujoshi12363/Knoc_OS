#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ulib.h"
#include "draw.h"
#include "wallpaper.h"

#define PANEL_X 80
#define PANEL_Y 80
#define PANEL_W 600
#define PANEL_H 380
#define SWATCH_Y 400
#define SWATCH_X 120
#define SWATCH_GAP 64

int main(int argc, char **argv)
{
    int wall = WALL_RIDGE;
    int accent = ACCENT_EMBER;
    int theme = THEME_GRAPHITE;
    int seconds = 10;

    for (int i = 1; i < argc; i++)
    {
        if (wallpaper_find(argv[i]) >= 0)
        {
            wall = wallpaper_find(argv[i]);
        }
        else if (accent_find(argv[i]) >= 0)
        {
            accent = accent_find(argv[i]);
        }
        else if (strcmp(argv[i], "paper") == 0)
        {
            theme = THEME_PAPER;
        }
        else if (strcmp(argv[i], "graphite") == 0)
        {
            theme = THEME_GRAPHITE;
        }
        else if (argv[i][0] >= '0' && argv[i][0] <= '9')
        {
            seconds = atoi(argv[i]);
        }
        else
        {
            printf("usage: gfx [ridge|dune|strata|grid] [ember|moss|sand|rose|mono] [graphite|paper] [seconds]\n");
            return 1;
        }
    }

    canvas_t screen;

    if (screen_open(&screen) != 0)
    {
        printf("gfx: no screen (start QEMU with a virtio-gpu device: make run-gui)\n");
        return 1;
    }

    font_t *ui = font_load("/fonts/HankenGrotesk-SemiBold.ttf");
    font_t *body = font_load("/fonts/HankenGrotesk-Regular.ttf");
    font_t *label = font_load("/fonts/MartianMono-Regular.ttf");
    font_t *mono = font_load("/fonts/JetBrainsMono-Regular.ttf");

    wallpaper_draw(&screen, wall, theme, accent);

    int dark = theme == THEME_GRAPHITE;
    uint32_t surface = dark ? 0x1A1D20 : 0xFBFBFC;
    uint32_t line = dark ? 0x2E3237 : 0xD8DCE0;
    uint32_t ink = dark ? 0xECEDEE : 0x111418;
    uint32_t ink2 = dark ? 0xA0A6AC : 0x535A62;
    uint32_t ink3 = dark ? 0x6C737A : 0x858C94;

    draw_round(&screen, PANEL_X + 2, PANEL_Y + 10, PANEL_W, PANEL_H, 8, 0x000000, 70);
    draw_round(&screen, PANEL_X, PANEL_Y, PANEL_W, PANEL_H, 7, surface, 255);
    draw_frame(&screen, PANEL_X, PANEL_Y, PANEL_W, PANEL_H, 7, line, 255);

    if (ui && body && label && mono)
    {
        draw_text(&screen, label, 11, PANEL_X + 32, PANEL_Y + 44, "KNOCOS · GRAPHICS · V0.33", ink3);
        draw_text(&screen, ui, 44, PANEL_X + 30, PANEL_Y + 104, "KnocOS Desktop", ink);
        draw_text(&screen, body, 17, PANEL_X + 32, PANEL_Y + 144, "Smooth text, drawn wallpapers and one accent colour.", ink2);
        draw_text(&screen, mono, 15, PANEL_X + 32, PANEL_Y + 190, "knoc:/home$ gfx ridge ember", ink2);

        char info[96];

        snprintf(info, sizeof(info), "%s · %s · %s", dark ? "GRAPHITE" : "PAPER", wallpaper_name(wall), accent_name(accent));

        for (char *p = info; *p; p++)
        {
            *p = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
        }

        draw_text(&screen, label, 11, PANEL_X + 32, PANEL_Y + 250, info, ink3);
    }

    for (int i = 0; i < ACCENT_COUNT; i++)
    {
        int cx = PANEL_X + SWATCH_X - 80 + i * SWATCH_GAP + 16;

        draw_circle(&screen, (float)cx, (float)(PANEL_Y + SWATCH_Y - 80), 16.0f, accent_color(i, theme), 255);

        if (i == accent)
        {
            draw_circle(&screen, (float)cx, (float)(PANEL_Y + SWATCH_Y - 80 + 26), 3.0f, ink, 255);
        }
    }

    screen_update(&screen, 0, 0, screen.width, screen.height);
    printf("gfx: %dx%d, %s wallpaper, %s accent, %s theme, fonts %s\n", screen.width, screen.height,
           wallpaper_name(wall), accent_name(accent), dark ? "graphite" : "paper",
           ui && body && label && mono ? "ok" : "missing (put third_party/fonts in /fonts)");

    sleep((unsigned long)seconds * 100);
    return 0;
}
