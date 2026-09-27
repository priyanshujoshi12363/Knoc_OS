#ifndef DESKTOP_H
#define DESKTOP_H

#include <stdint.h>
#include "../ulib.h"
#include "../draw.h"
#include "../wallpaper.h"

#define WINDOW_MAX 16
#define TITLE_H 34
#define RADIUS 7
#define STRIP_H 46
#define STRIP_GAP 14

typedef struct rect
{
    int x;
    int y;
    int w;
    int h;
} rect_t;

typedef struct theme
{
    int dark;
    int accent_index;
    int wall;
    int scale;
    uint32_t desk;
    uint32_t w;
    uint32_t w2;
    uint32_t w3;
    uint32_t ln;
    uint32_t ln2;
    uint32_t i;
    uint32_t i2;
    uint32_t i3;
    uint32_t a;
    uint32_t a_ink;
    uint32_t need;
    uint32_t ok;
    uint32_t bad;
    uint32_t term;
    uint32_t term_i;
} theme_t;

typedef struct window window_t;

typedef struct app
{
    const char *name;
    int width;
    int height;
    int (*open)(window_t *w, const char *arg);
    void (*draw)(window_t *w, canvas_t *c);
    void (*event)(window_t *w, const input_event_t *e, int x, int y);
    void (*tick)(window_t *w);
    void (*close)(window_t *w);
} app_t;

struct window
{
    int used;
    int id;
    const app_t *app;
    void *state;
    int x;
    int y;
    int w;
    int h;
    int minimized;
    int maximized;
    rect_t restore;
    char title[48];
    char crumb[160];
    canvas_t content;
    int dirty;
};

extern theme_t T;
extern font_t *font_ui;
extern font_t *font_bold;
extern font_t *font_label;
extern font_t *font_mono;
extern int screen_w;
extern int screen_h;

float S(float size);
int SI(int value);

void damage(int x, int y, int w, int h);
void damage_all(void);
void window_dirty(window_t *w);
window_t *window_open(const app_t *app, const char *arg);
void window_close(window_t *w);
void window_focus(window_t *w);
window_t *window_focused(void);
void window_set_title(window_t *w, const char *title, const char *crumb);
int window_count_app(const app_t *app);
window_t *window_find_app(const app_t *app);
void notify(const char *source, const char *title, const char *detail);
void theme_apply(int dark, int accent, int wall, int scale);
void settings_save(void);
void desktop_log(const char *text);
void open_path(const char *path);

void text_fit(char *out, int size, font_t *font, float px, const char *text, int width);
int draw_label(canvas_t *c, int x, int baseline, const char *text, uint32_t color);
void draw_icon(canvas_t *c, int kind, int x, int y, int size, uint32_t color);
int ui_button(canvas_t *c, int x, int y, const char *text, int primary);
void ui_field(canvas_t *c, int x, int y, int w, int h, const char *text, const char *placeholder, int focused);
int point_in(int px, int py, int x, int y, int w, int h);

enum
{
    ICON_HOME,
    ICON_DOWNLOAD,
    ICON_DOC,
    ICON_PICTURE,
    ICON_FOLDER,
    ICON_TERMINAL,
    ICON_SETTINGS,
    ICON_MONITOR,
    ICON_SEARCH,
    ICON_CLOSE,
    ICON_MIN,
    ICON_MAX,
    ICON_BACK,
    ICON_FORWARD,
    ICON_NETWORK,
    ICON_FILE,
    ICON_EDIT,
    ICON_DISK,
};

extern const app_t app_terminal;
extern const app_t app_files;
extern const app_t app_settings;
extern const app_t app_monitor;
extern const app_t app_viewer;
extern const app_t app_editor;

void knoc_init(void);
int knoc_bar_open(void);
void knoc_bar_toggle(void);
void knoc_bar_draw(canvas_t *view, int ox, int oy);
int knoc_bar_event(const input_event_t *e);
int assist_open(void);
int assist_width(void);
void assist_toggle(void);
void assist_draw(canvas_t *view, int ox, int oy);
int assist_event(const input_event_t *e);
void assist_tick(void);
void assist_ask(const char *question);
void presence_draw(canvas_t *c, int x, int y, int active);

#endif
