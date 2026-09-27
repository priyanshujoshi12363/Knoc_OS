#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "../third_party/stb/stb_truetype.h"

static const int extra_codes[] = {
    0x00A0, 0x00B0, 0x00B7, 0x00D7, 0x2014, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x2190, 0x2191, 0x2192,
    0x2193, 0x2500, 0x2502, 0x250C, 0x2510, 0x2514, 0x2518, 0x251C, 0x2524, 0x252C, 0x2534, 0x253C, 0x2550,
    0x2551, 0x2588, 0x2591, 0x2592, 0x2593, 0x25CF, 0x2713, 0x2717,
};

static int splash_font(const char *path, float em, const char *out_path)
{
    FILE *in = fopen(path, "rb");

    if (!in)
    {
        perror(path);
        return 1;
    }

    fseek(in, 0, SEEK_END);
    long size = ftell(in);
    fseek(in, 0, SEEK_SET);
    unsigned char *data = malloc(size);

    if (fread(data, 1, size, in) != (size_t)size)
    {
        return 1;
    }

    fclose(in);

    stbtt_fontinfo font;

    stbtt_InitFont(&font, data, stbtt_GetFontOffsetForIndex(data, 0));

    float scale = stbtt_ScaleForMappingEmToPixels(&font, em);
    int ascent, descent, gap;

    stbtt_GetFontVMetrics(&font, &ascent, &descent, &gap);

    int height = (int)lroundf((ascent - descent) * scale) + 2;
    int baseline = (int)lroundf(ascent * scale) + 1;
    int width = 0;

    for (int c = 32; c < 127; c++)
    {
        int advance, bearing;

        stbtt_GetCodepointHMetrics(&font, c, &advance, &bearing);
        width = (int)lroundf(advance * scale) + 4 > width ? (int)lroundf(advance * scale) + 4 : width;
    }

    FILE *out = fopen(out_path, "w");

    fprintf(out, "#ifndef SPLASH_FONT_INCLUDED\n#define SPLASH_FONT_INCLUDED\n\n#include <stdint.h>\n\n");
    fprintf(out, "#define SPLASH_FONT_W %d\n#define SPLASH_FONT_H %d\n\n", width, height);
    fprintf(out, "static const uint8_t splash_font_advance[95] = {");

    for (int c = 32; c < 127; c++)
    {
        int advance, bearing;

        stbtt_GetCodepointHMetrics(&font, c, &advance, &bearing);
        fprintf(out, "%s%d", c > 32 ? "," : "", (int)lroundf(advance * scale));
    }

    fprintf(out, "};\n\nstatic const uint8_t splash_font_alpha[95][SPLASH_FONT_W * SPLASH_FONT_H] = {\n");

    unsigned char *cell = calloc(width * height, 1);

    for (int c = 32; c < 127; c++)
    {
        int x0, y0, x1, y1;

        for (int j = 0; j < width * height; j++)
        {
            cell[j] = 0;
        }

        stbtt_GetCodepointBitmapBox(&font, c, scale, scale, &x0, &y0, &x1, &y1);

        int left = x0 > 0 ? x0 : 0;
        int top = baseline + y0 > 0 ? baseline + y0 : 0;

        if (left < width && top < height)
        {
            stbtt_MakeCodepointBitmap(&font, cell + top * width + left, width - left, height - top, width, scale, scale, c);
        }

        fprintf(out, "    {");

        for (int j = 0; j < width * height; j++)
        {
            fprintf(out, "%s%d", j ? "," : "", cell[j]);
        }

        fprintf(out, "},\n");
    }

    fprintf(out, "};\n\n#endif\n");
    fclose(out);
    printf("splash font: %dx%d cells\n", width, height);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 4 && argc != 5)
    {
        fprintf(stderr, "usage: mkfont FONT.ttf EM_PIXELS OUT.h [splash]\n");
        return 1;
    }

    if (argc == 5)
    {
        return splash_font(argv[1], (float)atof(argv[2]), argv[3]);
    }

    FILE *in = fopen(argv[1], "rb");

    if (!in)
    {
        perror(argv[1]);
        return 1;
    }

    fseek(in, 0, SEEK_END);
    long size = ftell(in);
    fseek(in, 0, SEEK_SET);
    unsigned char *data = malloc(size);

    if (fread(data, 1, size, in) != (size_t)size)
    {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }

    fclose(in);

    stbtt_fontinfo font;

    if (!stbtt_InitFont(&font, data, stbtt_GetFontOffsetForIndex(data, 0)))
    {
        fprintf(stderr, "%s is not a font\n", argv[1]);
        return 1;
    }

    float scale = stbtt_ScaleForMappingEmToPixels(&font, (float)atof(argv[2]));
    int ascent, descent, gap, advance, bearing;

    stbtt_GetFontVMetrics(&font, &ascent, &descent, &gap);
    stbtt_GetCodepointHMetrics(&font, 'M', &advance, &bearing);

    int width = (int)lroundf(advance * scale);
    int baseline = (int)lroundf(ascent * scale);
    int height = (int)lroundf((ascent - descent) * scale) + 1;
    int codes[256];
    int count = 0;

    for (int c = 32; c < 127; c++)
    {
        codes[count++] = c;
    }

    for (unsigned i = 0; i < sizeof(extra_codes) / sizeof(extra_codes[0]); i++)
    {
        if (stbtt_FindGlyphIndex(&font, extra_codes[i]))
        {
            codes[count++] = extra_codes[i];
        }
    }

    FILE *out = fopen(argv[3], "w");

    if (!out)
    {
        perror(argv[3]);
        return 1;
    }

    fprintf(out, "#ifndef CONSOLE_FONT_INCLUDED\n#define CONSOLE_FONT_INCLUDED\n\n#include <stdint.h>\n\n");
    fprintf(out, "#define CONSOLE_FONT_W %d\n#define CONSOLE_FONT_H %d\n#define CONSOLE_FONT_COUNT %d\n\n", width,
            height, count);
    fprintf(out, "static const uint16_t console_font_codes[CONSOLE_FONT_COUNT] = {");

    for (int i = 0; i < count; i++)
    {
        fprintf(out, "%s0x%04X", i ? "," : "", codes[i]);
    }

    fprintf(out, "};\n\nstatic const uint8_t console_font_alpha[CONSOLE_FONT_COUNT][CONSOLE_FONT_W * CONSOLE_FONT_H] = {\n");

    unsigned char *cell = calloc(width * height, 1);

    for (int i = 0; i < count; i++)
    {
        int x0, y0, x1, y1;
        int box = codes[i] >= 0x2500 && codes[i] <= 0x259F;

        for (int j = 0; j < width * height; j++)
        {
            cell[j] = 0;
        }

        if (box)
        {
            float sx = (float)width / (advance * scale) * scale;
            float sy = (float)height / ((ascent - descent) * scale) * scale;

            stbtt_GetCodepointBitmapBox(&font, codes[i], sx, sy, &x0, &y0, &x1, &y1);
            int top = (int)lroundf(ascent * sy);

            if (y0 + top < 0)
            {
                top = -y0;
            }

            stbtt_MakeCodepointBitmap(&font, cell + (top + y0) * width + (x0 > 0 ? x0 : 0), width - (x0 > 0 ? x0 : 0),
                                      height - (top + y0), width, sx, sy, codes[i]);
        }
        else
        {
            stbtt_GetCodepointBitmapBox(&font, codes[i], scale, scale, &x0, &y0, &x1, &y1);
            int left = x0 > 0 ? x0 : 0;
            int top = baseline + y0 > 0 ? baseline + y0 : 0;

            if (left < width && top < height)
            {
                stbtt_MakeCodepointBitmap(&font, cell + top * width + left, width - left, height - top, width, scale,
                                          scale, codes[i]);
            }
        }

        fprintf(out, "    {");

        for (int j = 0; j < width * height; j++)
        {
            fprintf(out, "%s%d", j ? "," : "", cell[j]);
        }

        fprintf(out, "},\n");
    }

    fprintf(out, "};\n\n#endif\n");
    fclose(out);
    printf("console font: %d glyphs, %dx%d cells\n", count, width, height);
    return 0;
}
