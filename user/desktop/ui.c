#include "desktop.h"
#include <string.h>
#include <stdio.h>

void text_fit(char *out, int size, font_t *font, float px, const char *text, int width)
{
    snprintf(out, (size_t)size, "%s", text);

    if (width <= 0)
    {
        out[0] = 0;
        return;
    }

    if (text_width(font, px, out) <= width)
    {
        return;
    }

    int length = (int)strlen(out);

    while (length > 0)
    {
        length--;

        while (length > 0 && (out[length] & 0xC0) == 0x80)
        {
            length--;
        }

        out[length] = 0;

        char trial[256];

        snprintf(trial, sizeof(trial), "%s…", out);

        if (text_width(font, px, trial) <= width)
        {
            snprintf(out, (size_t)size, "%s", trial);
            return;
        }
    }
}

int draw_label(canvas_t *c, int x, int baseline, const char *text, uint32_t color)
{
    char upper[96];
    int n = 0;

    for (; text[n] && n < 95; n++)
    {
        upper[n] = (char)(text[n] >= 'a' && text[n] <= 'z' ? text[n] - 32 : text[n]);
    }

    upper[n] = 0;
    return draw_text(c, font_label, S(9.5f), x, baseline, upper, color);
}

static void seg(canvas_t *c, int x, int y, float scale, float x0, float y0, float x1, float y1, uint32_t color)
{
    draw_line(c, (float)x + x0 * scale, (float)y + y0 * scale, (float)x + x1 * scale, (float)y + y1 * scale, color, 255);
}

void draw_icon(canvas_t *c, int kind, int x, int y, int size, uint32_t color)
{
    float s = (float)size / 16.0f;

    switch (kind)
    {
    case ICON_HOME:
        seg(c, x, y, s, 2.5f, 7.5f, 8, 3, color);
        seg(c, x, y, s, 8, 3, 13.5f, 7.5f, color);
        seg(c, x, y, s, 3.5f, 7, 3.5f, 13.5f, color);
        seg(c, x, y, s, 12.5f, 7, 12.5f, 13.5f, color);
        seg(c, x, y, s, 3.5f, 13.5f, 12.5f, 13.5f, color);
        break;
    case ICON_DOWNLOAD:
        seg(c, x, y, s, 8, 2.5f, 8, 10.5f, color);
        seg(c, x, y, s, 4.5f, 7, 8, 10.5f, color);
        seg(c, x, y, s, 11.5f, 7, 8, 10.5f, color);
        seg(c, x, y, s, 3, 13.5f, 13, 13.5f, color);
        break;
    case ICON_DOC:
    case ICON_FILE:
        seg(c, x, y, s, 4, 2, 9.5f, 2, color);
        seg(c, x, y, s, 9.5f, 2, 12, 4.5f, color);
        seg(c, x, y, s, 12, 4.5f, 12, 14, color);
        seg(c, x, y, s, 12, 14, 4, 14, color);
        seg(c, x, y, s, 4, 14, 4, 2, color);
        break;
    case ICON_PICTURE:
        seg(c, x, y, s, 2, 3, 14, 3, color);
        seg(c, x, y, s, 14, 3, 14, 13, color);
        seg(c, x, y, s, 14, 13, 2, 13, color);
        seg(c, x, y, s, 2, 13, 2, 3, color);
        seg(c, x, y, s, 2, 11, 5.5f, 8, color);
        seg(c, x, y, s, 5.5f, 8, 8.5f, 10.5f, color);
        seg(c, x, y, s, 8.5f, 10.5f, 11, 8, color);
        seg(c, x, y, s, 11, 8, 14, 11, color);
        break;
    case ICON_FOLDER:
        seg(c, x, y, s, 2, 4, 6.5f, 4, color);
        seg(c, x, y, s, 6.5f, 4, 8, 5.5f, color);
        seg(c, x, y, s, 8, 5.5f, 14, 5.5f, color);
        seg(c, x, y, s, 14, 5.5f, 14, 13, color);
        seg(c, x, y, s, 14, 13, 2, 13, color);
        seg(c, x, y, s, 2, 13, 2, 4, color);
        break;
    case ICON_TERMINAL:
        seg(c, x, y, s, 3, 4, 7, 8, color);
        seg(c, x, y, s, 7, 8, 3, 12, color);
        seg(c, x, y, s, 8.5f, 12.5f, 13, 12.5f, color);
        break;
    case ICON_SETTINGS:
        draw_circle(c, (float)x + 8 * s, (float)y + 8 * s, 5.5f * s, color, 255);
        draw_circle(c, (float)x + 8 * s, (float)y + 8 * s, 4.2f * s, T.w, 255);
        draw_circle(c, (float)x + 8 * s, (float)y + 8 * s, 1.8f * s, color, 255);
        break;
    case ICON_MONITOR:
        seg(c, x, y, s, 2, 3, 14, 3, color);
        seg(c, x, y, s, 14, 3, 14, 11, color);
        seg(c, x, y, s, 14, 11, 2, 11, color);
        seg(c, x, y, s, 2, 11, 2, 3, color);
        seg(c, x, y, s, 5.5f, 14, 10.5f, 14, color);
        seg(c, x, y, s, 8, 11, 8, 14, color);
        break;
    case ICON_SEARCH:
        draw_circle(c, (float)x + 7 * s, (float)y + 7 * s, 4.8f * s, color, 255);
        draw_circle(c, (float)x + 7 * s, (float)y + 7 * s, 3.4f * s, T.w, 255);
        seg(c, x, y, s, 10.5f, 10.5f, 14, 14, color);
        break;
    case ICON_CLOSE:
        seg(c, x, y, s, 3, 3, 13, 13, color);
        seg(c, x, y, s, 13, 3, 3, 13, color);
        break;
    case ICON_MIN:
        seg(c, x, y, s, 3, 8, 13, 8, color);
        break;
    case ICON_MAX:
        seg(c, x, y, s, 3, 3, 13, 3, color);
        seg(c, x, y, s, 13, 3, 13, 13, color);
        seg(c, x, y, s, 13, 13, 3, 13, color);
        seg(c, x, y, s, 3, 13, 3, 3, color);
        break;
    case ICON_BACK:
        seg(c, x, y, s, 10, 3, 5, 8, color);
        seg(c, x, y, s, 5, 8, 10, 13, color);
        break;
    case ICON_FORWARD:
        seg(c, x, y, s, 6, 3, 11, 8, color);
        seg(c, x, y, s, 11, 8, 6, 13, color);
        break;
    case ICON_NETWORK:
        seg(c, x, y, s, 1.5f, 6, 8, 2, color);
        seg(c, x, y, s, 8, 2, 14.5f, 6, color);
        seg(c, x, y, s, 4, 8.8f, 8, 6.5f, color);
        seg(c, x, y, s, 8, 6.5f, 12, 8.8f, color);
        draw_circle(c, (float)x + 8 * s, (float)y + 12 * s, 1.4f * s, color, 255);
        break;
    case ICON_EDIT:
        seg(c, x, y, s, 3, 13, 4, 10, color);
        seg(c, x, y, s, 4, 10, 11, 3, color);
        seg(c, x, y, s, 11, 3, 13, 5, color);
        seg(c, x, y, s, 13, 5, 6, 12, color);
        seg(c, x, y, s, 6, 12, 3, 13, color);
        break;
    case ICON_DISK:
        seg(c, x, y, s, 2, 4, 14, 4, color);
        seg(c, x, y, s, 14, 4, 14, 12, color);
        seg(c, x, y, s, 14, 12, 2, 12, color);
        seg(c, x, y, s, 2, 12, 2, 4, color);
        draw_circle(c, (float)x + 11.5f * s, (float)y + 8 * s, 1.2f * s, color, 255);
        break;
    default:
        break;
    }
}

int ui_button(canvas_t *c, int x, int y, const char *text, int primary)
{
    int w = text_width(font_bold, S(12), text) + 22;
    int h = SI(28);

    if (primary)
    {
        draw_round(c, x, y, w, h, 4, T.i, 255);
        draw_text(c, font_bold, S(12), x + 11, y + h / 2 + SI(4), text, T.w);
    }
    else
    {
        draw_round(c, x, y, w, h, 4, T.w, 255);
        draw_frame(c, x, y, w, h, 4, T.ln2, 255);
        draw_text(c, font_bold, S(12), x + 11, y + h / 2 + SI(4), text, T.i);
    }

    return w;
}

void ui_field(canvas_t *c, int x, int y, int w, int h, const char *text, const char *placeholder, int focused)
{
    draw_round(c, x, y, w, h, 5, T.w, 255);
    draw_frame(c, x, y, w, h, 5, focused ? T.a : T.ln2, 255);

    char fitted[256];
    int baseline = y + h / 2 + SI(5);

    if (text && text[0])
    {
        text_fit(fitted, sizeof(fitted), font_ui, S(13), text, w - 20);
        int tw = draw_text(c, font_ui, S(13), x + 9, baseline, fitted, T.i);

        if (focused)
        {
            draw_fill(c, x + 10 + tw, y + 6, 2, h - 12, T.a);
        }
    }
    else
    {
        draw_text(c, font_ui, S(13), x + 9, baseline, placeholder ? placeholder : "", T.i3);

        if (focused)
        {
            draw_fill(c, x + 9, y + 6, 2, h - 12, T.a);
        }
    }
}
