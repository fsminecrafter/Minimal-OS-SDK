#ifndef MINIMALOS_EASYUI_H
#define MINIMALOS_EASYUI_H

/*
 * easyui - widgets for mosWM apps.
 *
 *     #include "easyui.h"
 *
 *     static void clicked(int value) { mos_puts("clicked\n"); }
 *
 *     int main(void) {
 *         easyui.init("Hello", 320, 200);
 *         int button1 = easyui.createbutton(20, 20, 200, 40, "Click me!");
 *         easyui.recallfunc(button1, ONCLICK, clicked);
 *         return easyui.run();
 *     }
 *
 * One window per program, holding buttons, labels, sliders, checkboxes,
 * radio buttons, toggles, text inputs, progress bars, dropdowns, list
 * boxes, panels, images and canvases, plus file and folder pickers. Text is
 * anti-aliased Inter, drawn with mosWM's OpenType renderer (easyui_text.h)
 * from the fonts mosWM installs; the 8x8 bitmap font is the fallback. Any
 * widget can have its own font, text size, colours, border, corner radius
 * and padding. Colours follow the MOSdesk theme chosen in Settings >
 * Appearance (the accent colour in 0:/programs/mosdesk/settings.dat), and
 * widgets take the "2000s Aero" look when that window style is chosen;
 * both change while the app runs.
 *
 * Everything is reached through the `easyui` object. Widgets are ints
 * (0 means "failed" for create calls, and "the window" for recallfunc).
 * Callbacks take one int, the event's value; easyui.current() tells a
 * shared callback which widget fired.
 *
 * The implementation is in this header. In a program with several source
 * files, include it normally in one of them and put
 *     #define EASYUI_NO_IMPLEMENTATION
 * before the include in the others. Full reference: docs/easyui.md.
 */

#include <stdint.h>
#include <stddef.h>
#include "window.h"
#include "minimalos.h"
#include "stdlib.h"
#include "string.h"

#define EASYUI_MAX_WIDGETS 128
#define EASYUI_TEXT_MAX 128
#define EASYUI_MAX_TIMERS 8
#define EASYUI_MAX_FONTS 8          /* fonts addfont() can load */
#define EASYUI_PATH_MAX 256         /* paths the pickers return */

#define EASYUI_WINDOW 0

typedef int easyui_widget;
typedef void (*easyui_callback)(int value);

/* Events for recallfunc. */
enum {
    EASYUI_ONPRESS = 0,   /* mouse button (or Space/Enter) went down on it */
    EASYUI_ONRELEASE,     /* ... and came up again, wherever the pointer is */
    EASYUI_ONCLICK,       /* pressed and released on the widget */
    EASYUI_ONCE,          /* like ONCLICK, but only the first time */
    EASYUI_ONCHANGE,      /* its value changed (slider, checkbox, text ...) */
    EASYUI_ONSUBMIT,      /* Enter in a text input, double-click in a list */
    EASYUI_ONHOVER,       /* the pointer moved onto it */
    EASYUI_ONLEAVE,       /* the pointer moved off it */
    EASYUI_ONFOCUS,       /* it got keyboard focus */
    EASYUI_ONBLUR,        /* it lost keyboard focus */
    EASYUI_ONKEY,         /* a key was pressed while it had focus */
    EASYUI_ONDRAW,        /* canvas: draw your content now */
    EASYUI_ONFRAME,       /* window: once per frame, about 60 times a second */
    EASYUI_ONCLOSE,       /* window: the close button was clicked */
    EASYUI_ONRESIZE,      /* window: it changed size */
    EASYUI_ONWHEEL,       /* canvas: mouse wheel over it; value: +1 up, -1 down */
    EASYUI_EVENT_COUNT
};

#ifndef EASYUI_NO_SHORT_NAMES
#define ONPRESS   EASYUI_ONPRESS
#define ONRELEASE EASYUI_ONRELEASE
#define ONCLICK   EASYUI_ONCLICK
#define ONCE      EASYUI_ONCE
#define ONCHANGE  EASYUI_ONCHANGE
#define ONSUBMIT  EASYUI_ONSUBMIT
#define ONHOVER   EASYUI_ONHOVER
#define ONLEAVE   EASYUI_ONLEAVE
#define ONFOCUS   EASYUI_ONFOCUS
#define ONBLUR    EASYUI_ONBLUR
#define ONKEY     EASYUI_ONKEY
#define ONDRAW    EASYUI_ONDRAW
#define ONFRAME   EASYUI_ONFRAME
#define ONWHEEL   EASYUI_ONWHEEL
#define ONCLOSE   EASYUI_ONCLOSE
#define ONRESIZE  EASYUI_ONRESIZE
#endif

/* Widget kinds, as returned by easyui.type(). */
enum {
    EASYUI_NONE = 0, EASYUI_BUTTON, EASYUI_LABEL, EASYUI_SLIDER,
    EASYUI_CHECKBOX, EASYUI_RADIO, EASYUI_TOGGLE, EASYUI_TEXTINPUT,
    EASYUI_PROGRESS, EASYUI_DROPDOWN, EASYUI_LISTBOX, EASYUI_PANEL,
    EASYUI_IMAGE, EASYUI_CANVAS, EASYUI_SEPARATOR
};

/* Button styles for setstyle(). */
enum { EASYUI_PRIMARY = 0, EASYUI_SECONDARY, EASYUI_DANGER, EASYUI_FLAT };

/* Text alignment for setalign(). */
enum { EASYUI_ALIGN_LEFT = 0, EASYUI_ALIGN_CENTER, EASYUI_ALIGN_RIGHT };

/* Fonts for setfont(). EASYUI_FONT_DEFAULT picks by text size (Inter
 * Regular at size 1, Inter Medium above); addfont() returns more, starting
 * at EASYUI_FONT_CUSTOM. */
enum {
    EASYUI_FONT_DEFAULT = 0, EASYUI_FONT_REGULAR, EASYUI_FONT_MEDIUM,
    EASYUI_FONT_BITMAP, EASYUI_FONT_CUSTOM
};

/* Looks for setlook(): follow MOSdesk's window style, or force one. */
enum { EASYUI_LOOK_AUTO = 0, EASYUI_LOOK_MODERN, EASYUI_LOOK_AERO };

/* Anchors for setanchor(): which window edges a widget keeps its distance
 * to when the window is resized. LEFT|RIGHT stretches it. */
#define EASYUI_ANCHOR_LEFT   1u
#define EASYUI_ANCHOR_TOP    2u
#define EASYUI_ANCHOR_RIGHT  4u
#define EASYUI_ANCHOR_BOTTOM 8u

/* Key codes in ONKEY values (USB HID usage codes, as mosWM sends them).
 * The value is key | (modifiers << 8); modifiers are
 * SYSCALL_INPUT_MOD_SHIFT / _CTRL / _ALT. */
#define EASYUI_KEY_ENTER     40
#define EASYUI_KEY_ESCAPE    41
#define EASYUI_KEY_BACKSPACE 42
#define EASYUI_KEY_TAB       43
#define EASYUI_KEY_SPACE     44
#define EASYUI_KEY_HOME      74
#define EASYUI_KEY_DELETE    76
#define EASYUI_KEY_END       77
#define EASYUI_KEY_RIGHT     79
#define EASYUI_KEY_LEFT      80
#define EASYUI_KEY_DOWN      81
#define EASYUI_KEY_UP        82

typedef struct {
    uint32_t background;      /* window content */
    uint32_t surface;         /* text inputs, lists, panels */
    uint32_t border;
    uint32_t text;
    uint32_t muted;           /* placeholders, panel titles */
    uint32_t button;          /* secondary buttons */
    uint32_t button_hover;
    uint32_t button_pressed;
    uint32_t accent;          /* from MOSdesk, or setaccent() */
    uint32_t accent_hover;
    uint32_t accent_pressed;
    uint32_t accent_text;     /* text on accent */
    uint32_t track;           /* slider, progress and toggle tracks */
    uint32_t disabled;
    uint32_t disabled_text;
    uint32_t danger;
} easyui_theme_t;

typedef struct {
    /* --- window and main loop --- */
    int  (*init)(const char* title, int width, int height);
    int  (*run)(void);
    int  (*update)(void);
    void (*quit)(void);
    void (*settitle)(const char* title);
    int  (*width)(void);
    int  (*height)(void);
    uint32_t (*window)(void);
    void (*redraw)(void);
    /* --- theme --- */
    void (*setdarkmode)(int on);
    void (*setaccent)(uint32_t argb);
    uint32_t (*accent)(void);
    const easyui_theme_t* (*theme)(void);
    /* --- text --- */
    int  (*loadfont)(const char* path, int bold);
    void (*setsmoothtext)(int on);
    int  (*smoothtext)(void);
    /* --- creating widgets --- */
    easyui_widget (*createbutton)(int x, int y, int w, int h, const char* text);
    easyui_widget (*createlabel)(int x, int y, int w, int h, const char* text);
    easyui_widget (*createslider)(int x, int y, int w, int h, int min, int max, int value);
    easyui_widget (*createcheckbox)(int x, int y, int w, int h, const char* text, int checked);
    easyui_widget (*createradio)(int x, int y, int w, int h, const char* text, int group);
    easyui_widget (*createtoggle)(int x, int y, int w, int h, const char* text, int on);
    easyui_widget (*createtextinput)(int x, int y, int w, int h, const char* placeholder);
    easyui_widget (*createprogress)(int x, int y, int w, int h, int max);
    easyui_widget (*createdropdown)(int x, int y, int w, int h);
    easyui_widget (*createlistbox)(int x, int y, int w, int h);
    easyui_widget (*createpanel)(int x, int y, int w, int h, const char* title);
    easyui_widget (*createimage)(int x, int y, int w, int h, const uint32_t* pixels, int image_w, int image_h);
    easyui_widget (*createcanvas)(int x, int y, int w, int h);
    easyui_widget (*createseparator)(int x, int y, int w);
    void (*remove)(easyui_widget w);
    /* --- events --- */
    int  (*recallfunc)(easyui_widget w, int event, easyui_callback fn);
    easyui_widget (*current)(void);
    /* --- widget properties --- */
    void (*settext)(easyui_widget w, const char* text);
    const char* (*gettext)(easyui_widget w);
    void (*setvalue)(easyui_widget w, int value);
    int  (*getvalue)(easyui_widget w);
    void (*setrange)(easyui_widget w, int min, int max);
    void (*setstep)(easyui_widget w, int step);
    int  (*additem)(easyui_widget w, const char* text);
    void (*clearitems)(easyui_widget w);
    const char* (*getitem)(easyui_widget w, int index);
    int  (*itemcount)(easyui_widget w);
    void (*setenabled)(easyui_widget w, int enabled);
    void (*setvisible)(easyui_widget w, int visible);
    void (*setposition)(easyui_widget w, int x, int y);
    void (*setsize)(easyui_widget w, int width, int height);
    void (*setanchor)(easyui_widget w, uint32_t anchors);
    void (*setcolor)(easyui_widget w, uint32_t argb);
    void (*settextsize)(easyui_widget w, int size);
    void (*setalign)(easyui_widget w, int align);
    void (*setstyle)(easyui_widget w, int style);
    void (*setpassword)(easyui_widget w, int hidden);
    void (*setmaxlength)(easyui_widget w, int length);
    void (*setimage)(easyui_widget w, const uint32_t* pixels, int image_w, int image_h);
    void (*setfocus)(easyui_widget w);
    easyui_widget (*focused)(void);
    int  (*type)(easyui_widget w);
    int  (*getx)(easyui_widget w);
    int  (*gety)(easyui_widget w);
    int  (*getwidth)(easyui_widget w);
    int  (*getheight)(easyui_widget w);
    /* --- timers and dialogs --- */
    int  (*settimer)(int interval_ms, easyui_callback fn);
    void (*stoptimer)(int timer);
    void (*messagebox)(const char* title, const char* text);
    /* --- drawing (inside ONDRAW, or on the window between frames) --- */
    uint32_t* (*pixels)(void);
    void (*fillrect)(int x, int y, int w, int h, uint32_t argb);
    void (*drawrect)(int x, int y, int w, int h, uint32_t argb);
    void (*fillroundrect)(int x, int y, int w, int h, int radius, uint32_t argb);
    void (*fillcircle)(int cx, int cy, int radius, uint32_t argb);
    void (*drawline)(int x1, int y1, int x2, int y2, uint32_t argb);
    void (*drawtext)(int x, int y, const char* text, uint32_t argb, int size);
    int  (*textwidth)(const char* text, int size);
    void (*drawpixels)(int x, int y, const uint32_t* pixels, int w, int h);
    /* --- styling: any widget, or EASYUI_WINDOW for every widget --- */
    int  (*addfont)(const char* path);
    void (*setfont)(easyui_widget w, int font);
    void (*setfontsize)(easyui_widget w, int pixels);
    void (*settextcolor)(easyui_widget w, uint32_t argb);
    void (*setbackground)(easyui_widget w, uint32_t argb);
    void (*setborder)(easyui_widget w, uint32_t argb, int width);
    void (*setradius)(easyui_widget w, int radius);
    void (*setpadding)(easyui_widget w, int pixels);
    void (*copystyle)(easyui_widget to, easyui_widget from);
    void (*drawtextfont)(int x, int y, const char* text, uint32_t argb, int font, int pixels);
    int  (*textwidthfont)(const char* text, int font, int pixels);
    void (*setlook)(int look);
    int  (*look)(void);
    /* --- file and folder pickers --- */
    int  (*pickfile)(const char* title, const char* folder, const char* extensions, easyui_callback done);
    int  (*pickfolder)(const char* title, const char* folder, easyui_callback done);
    int  (*savefile)(const char* title, const char* folder, const char* name, easyui_callback done);
    const char* (*pickedpath)(void);
    /* --- pointer: window coordinates, buttons bit 0 = left (-1, -1 when
     * it is outside) --- */
    void (*mouse)(int* x, int* y, int* buttons);
} easyui_api_t;

#ifdef __cplusplus
extern "C" {
#endif
extern const easyui_api_t easyui;
#ifdef __cplusplus
}
#endif

#ifndef EASYUI_NO_IMPLEMENTATION

/* ======================================================================
 *  Implementation
 * ====================================================================== */

/* 8x8 glyphs for ' ' .. '~': the public-domain font8x8_basic set (the font
 * the Minimal-OS terminal and mosWM's fallback text use). Bit 0 is the
 * leftmost pixel. */
static const uint8_t eui_font[95][8] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},  /* ' ' */
    {0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00},  /* '!' */
    {0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},  /* '"' */
    {0x36, 0x36, 0x7F, 0x36, 0x7F, 0x36, 0x36, 0x00},  /* '#' */
    {0x0C, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x0C, 0x00},  /* '$' */
    {0x00, 0x63, 0x33, 0x18, 0x0C, 0x66, 0x63, 0x00},  /* '%' */
    {0x1C, 0x36, 0x1C, 0x6E, 0x3B, 0x33, 0x6E, 0x00},  /* '&' */
    {0x06, 0x06, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00},  /* 0x27 */
    {0x18, 0x0C, 0x06, 0x06, 0x06, 0x0C, 0x18, 0x00},  /* '(' */
    {0x06, 0x0C, 0x18, 0x18, 0x18, 0x0C, 0x06, 0x00},  /* ')' */
    {0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00},  /* '*' */
    {0x00, 0x0C, 0x0C, 0x3F, 0x0C, 0x0C, 0x00, 0x00},  /* '+' */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x06},  /* ',' */
    {0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x00},  /* '-' */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00},  /* '.' */
    {0x60, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x01, 0x00},  /* '/' */
    {0x3E, 0x63, 0x73, 0x7B, 0x6F, 0x67, 0x3E, 0x00},  /* '0' */
    {0x0C, 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x3F, 0x00},  /* '1' */
    {0x1E, 0x33, 0x30, 0x1C, 0x06, 0x33, 0x3F, 0x00},  /* '2' */
    {0x1E, 0x33, 0x30, 0x1C, 0x30, 0x33, 0x1E, 0x00},  /* '3' */
    {0x38, 0x3C, 0x36, 0x33, 0x7F, 0x30, 0x78, 0x00},  /* '4' */
    {0x3F, 0x03, 0x1F, 0x30, 0x30, 0x33, 0x1E, 0x00},  /* '5' */
    {0x1C, 0x06, 0x03, 0x1F, 0x33, 0x33, 0x1E, 0x00},  /* '6' */
    {0x3F, 0x33, 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x00},  /* '7' */
    {0x1E, 0x33, 0x33, 0x1E, 0x33, 0x33, 0x1E, 0x00},  /* '8' */
    {0x1E, 0x33, 0x33, 0x3E, 0x30, 0x18, 0x0E, 0x00},  /* '9' */
    {0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x00},  /* ':' */
    {0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x06},  /* ';' */
    {0x18, 0x0C, 0x06, 0x03, 0x06, 0x0C, 0x18, 0x00},  /* '<' */
    {0x00, 0x00, 0x3F, 0x00, 0x00, 0x3F, 0x00, 0x00},  /* '=' */
    {0x06, 0x0C, 0x18, 0x30, 0x18, 0x0C, 0x06, 0x00},  /* '>' */
    {0x1E, 0x33, 0x30, 0x18, 0x0C, 0x00, 0x0C, 0x00},  /* '?' */
    {0x3E, 0x63, 0x7B, 0x7B, 0x7B, 0x03, 0x1E, 0x00},  /* '@' */
    {0x0C, 0x1E, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x00},  /* 'A' */
    {0x3F, 0x66, 0x66, 0x3E, 0x66, 0x66, 0x3F, 0x00},  /* 'B' */
    {0x3C, 0x66, 0x03, 0x03, 0x03, 0x66, 0x3C, 0x00},  /* 'C' */
    {0x1F, 0x36, 0x66, 0x66, 0x66, 0x36, 0x1F, 0x00},  /* 'D' */
    {0x7F, 0x46, 0x16, 0x1E, 0x16, 0x46, 0x7F, 0x00},  /* 'E' */
    {0x7F, 0x46, 0x16, 0x1E, 0x16, 0x06, 0x0F, 0x00},  /* 'F' */
    {0x3C, 0x66, 0x03, 0x03, 0x73, 0x66, 0x7C, 0x00},  /* 'G' */
    {0x33, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x33, 0x00},  /* 'H' */
    {0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},  /* 'I' */
    {0x78, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E, 0x00},  /* 'J' */
    {0x67, 0x66, 0x36, 0x1E, 0x36, 0x66, 0x67, 0x00},  /* 'K' */
    {0x0F, 0x06, 0x06, 0x06, 0x46, 0x66, 0x7F, 0x00},  /* 'L' */
    {0x63, 0x77, 0x7F, 0x7F, 0x6B, 0x63, 0x63, 0x00},  /* 'M' */
    {0x63, 0x67, 0x6F, 0x7B, 0x73, 0x63, 0x63, 0x00},  /* 'N' */
    {0x1C, 0x36, 0x63, 0x63, 0x63, 0x36, 0x1C, 0x00},  /* 'O' */
    {0x3F, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x0F, 0x00},  /* 'P' */
    {0x1E, 0x33, 0x33, 0x33, 0x3B, 0x1E, 0x38, 0x00},  /* 'Q' */
    {0x3F, 0x66, 0x66, 0x3E, 0x36, 0x66, 0x67, 0x00},  /* 'R' */
    {0x1E, 0x33, 0x07, 0x0E, 0x38, 0x33, 0x1E, 0x00},  /* 'S' */
    {0x3F, 0x2D, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},  /* 'T' */
    {0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x3F, 0x00},  /* 'U' */
    {0x33, 0x33, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00},  /* 'V' */
    {0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00},  /* 'W' */
    {0x63, 0x63, 0x36, 0x1C, 0x1C, 0x36, 0x63, 0x00},  /* 'X' */
    {0x33, 0x33, 0x33, 0x1E, 0x0C, 0x0C, 0x1E, 0x00},  /* 'Y' */
    {0x7F, 0x63, 0x31, 0x18, 0x4C, 0x66, 0x7F, 0x00},  /* 'Z' */
    {0x1E, 0x06, 0x06, 0x06, 0x06, 0x06, 0x1E, 0x00},  /* '[' */
    {0x03, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x40, 0x00},  /* 0x5C */
    {0x1E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1E, 0x00},  /* ']' */
    {0x08, 0x1C, 0x36, 0x63, 0x00, 0x00, 0x00, 0x00},  /* '^' */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF},  /* '_' */
    {0x0C, 0x0C, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00},  /* '`' */
    {0x00, 0x00, 0x1E, 0x30, 0x3E, 0x33, 0x6E, 0x00},  /* 'a' */
    {0x07, 0x06, 0x06, 0x3E, 0x66, 0x66, 0x3B, 0x00},  /* 'b' */
    {0x00, 0x00, 0x1E, 0x33, 0x03, 0x33, 0x1E, 0x00},  /* 'c' */
    {0x38, 0x30, 0x30, 0x3E, 0x33, 0x33, 0x6E, 0x00},  /* 'd' */
    {0x00, 0x00, 0x1E, 0x33, 0x3F, 0x03, 0x1E, 0x00},  /* 'e' */
    {0x1C, 0x36, 0x06, 0x0F, 0x06, 0x06, 0x0F, 0x00},  /* 'f' */
    {0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x1F},  /* 'g' */
    {0x07, 0x06, 0x36, 0x6E, 0x66, 0x66, 0x67, 0x00},  /* 'h' */
    {0x0C, 0x00, 0x0E, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},  /* 'i' */
    {0x30, 0x00, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E},  /* 'j' */
    {0x07, 0x06, 0x66, 0x36, 0x1E, 0x36, 0x67, 0x00},  /* 'k' */
    {0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},  /* 'l' */
    {0x00, 0x00, 0x33, 0x7F, 0x7F, 0x6B, 0x63, 0x00},  /* 'm' */
    {0x00, 0x00, 0x1F, 0x33, 0x33, 0x33, 0x33, 0x00},  /* 'n' */
    {0x00, 0x00, 0x1E, 0x33, 0x33, 0x33, 0x1E, 0x00},  /* 'o' */
    {0x00, 0x00, 0x3B, 0x66, 0x66, 0x3E, 0x06, 0x0F},  /* 'p' */
    {0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x78},  /* 'q' */
    {0x00, 0x00, 0x3B, 0x6E, 0x66, 0x06, 0x0F, 0x00},  /* 'r' */
    {0x00, 0x00, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x00},  /* 's' */
    {0x08, 0x0C, 0x3E, 0x0C, 0x0C, 0x2C, 0x18, 0x00},  /* 't' */
    {0x00, 0x00, 0x33, 0x33, 0x33, 0x33, 0x6E, 0x00},  /* 'u' */
    {0x00, 0x00, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00},  /* 'v' */
    {0x00, 0x00, 0x63, 0x6B, 0x7F, 0x7F, 0x36, 0x00},  /* 'w' */
    {0x00, 0x00, 0x63, 0x36, 0x1C, 0x36, 0x63, 0x00},  /* 'x' */
    {0x00, 0x00, 0x33, 0x33, 0x33, 0x3E, 0x30, 0x1F},  /* 'y' */
    {0x00, 0x00, 0x3F, 0x19, 0x0C, 0x26, 0x3F, 0x00},  /* 'z' */
    {0x38, 0x0C, 0x0C, 0x07, 0x0C, 0x0C, 0x38, 0x00},  /* '{' */
    {0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00},  /* '|' */
    {0x07, 0x0C, 0x0C, 0x38, 0x0C, 0x0C, 0x07, 0x00},  /* '}' */
    {0x6E, 0x3B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},  /* '~' */
};

/* Smooth text: ttrend_outline, the OpenType renderer mosWM draws with. */
#include "easyui_text.h"

#define EASYUI_FONT_REGULAR_PATH "0:/programs/mosWM/assets/fonts/Inter-Regular.otf"
#define EASYUI_FONT_MEDIUM_PATH  "0:/programs/mosWM/assets/fonts/Inter-Medium.otf"

static ttrend_outline_font_t eui_ttf_font[2];   /* [0] regular, [1] medium */
static void* eui_ttf_data[2];
static int eui_ttf_ok[2], eui_smooth = 1, eui_fonts_tried;
/* Fonts from addfont(): font EASYUI_FONT_CUSTOM + i is eui_cfont[i]. */
static ttrend_outline_font_t eui_cfont[EASYUI_MAX_FONTS];
static void* eui_cfont_data[EASYUI_MAX_FONTS];
static int eui_cfont_count;
/* The font and pixel size text is measured and drawn with right now: the
 * widget being painted (or measured), else the window's defaults. */
static int eui_tfont, eui_tpx;

typedef struct {
    uint8_t used, type, visible, enabled;
    uint8_t style, align, password, hovered;
    int32_t x, y, w, h, size;
    uint32_t anchor;
    int32_t margin_right, margin_bottom;
    char text[EASYUI_TEXT_MAX];
    char placeholder[64];
    int32_t value, min, max, step, group;
    int32_t scroll, cursor, maxlength, item_hover;
    uint32_t color;
    /* Style: 0 / unset means "the window's default, else the theme". */
    int32_t font, font_px;
    uint32_t text_color, background, border_color;
    int32_t border_width, radius, padding;
    uint8_t size_set, has_border, has_radius, has_padding;
    const uint32_t* image;
    int32_t image_w, image_h;
    char** items;
    int32_t count, capacity;
    easyui_callback cb[EASYUI_EVENT_COUNT];
} eui_widget_t;

typedef struct { int32_t interval, next; easyui_callback fn; } eui_timer_t;

static eui_widget_t eui_w[EASYUI_MAX_WIDGETS + 1];   /* [0] is the window */
static eui_timer_t eui_timers[EASYUI_MAX_TIMERS];
static easyui_theme_t eui_th;
static uint32_t eui_win, *eui_px;
static int32_t eui_W, eui_H;
static int eui_running, eui_dirty, eui_dark;
static uint32_t eui_accent_override, eui_settings_accent = 0xFF3DAEE9u;
static long eui_last_poll = -100000, eui_last_click_time;
static int eui_hover, eui_pressed, eui_focus, eui_open, eui_cur, eui_last_click_item = -1;
static int32_t eui_mx = -1, eui_my = -1;
static uint32_t eui_buttons;
static int eui_look, eui_aero, eui_settings_style;
static int eui_caret_on;
static long eui_caret_time;
static int32_t eui_cx0, eui_cy0, eui_cx1, eui_cy1;   /* clip rectangle */
static int eui_msg_active, eui_msg_hover;
static char eui_msg_title[64], eui_msg_text[256];

/* ---------------------------------------------------------------------
 *  Colours and theme
 * --------------------------------------------------------------------- */
static uint32_t eui_mix(uint32_t a, uint32_t b, uint32_t t) {   /* t/255 of b */
    uint32_t r = (((a >> 16) & 255u) * (255u - t) + ((b >> 16) & 255u) * t) / 255u;
    uint32_t g = (((a >> 8) & 255u) * (255u - t) + ((b >> 8) & 255u) * t) / 255u;
    uint32_t bl = ((a & 255u) * (255u - t) + (b & 255u) * t) / 255u;
    return 0xFF000000u | (r << 16) | (g << 8) | bl;
}

static void eui_make_theme(void) {
    uint32_t a = 0xFF000000u | ((eui_accent_override ? eui_accent_override : eui_settings_accent) & 0x00FFFFFFu);
    eui_aero = eui_look == EASYUI_LOOK_AERO ||
               (eui_look == EASYUI_LOOK_AUTO && eui_settings_style == 1);
    if (eui_aero && !eui_dark) {
        /* Windows 7 era: light grey dialogs, black text, grey glass buttons */
        eui_th.background = 0xFFF0F0F0u; eui_th.surface = 0xFFFFFFFFu;
        eui_th.border = 0xFFABADB3u;     eui_th.text = 0xFF000000u;
        eui_th.muted = 0xFF6D6D6Du;      eui_th.button = 0xFFE5E5E5u;
        eui_th.button_hover = 0xFFDCECFCu; eui_th.button_pressed = 0xFFC4E5F6u;
        eui_th.track = 0xFFE6E6E6u;      eui_th.disabled = 0xFFF4F4F4u;
        eui_th.disabled_text = 0xFF838383u;
    } else if (eui_dark) {
        /* mosWM's menus and Files window */
        eui_th.background = 0xFF1F2428u; eui_th.surface = 0xFF2A3036u;
        eui_th.border = 0xFF3A4249u;     eui_th.text = 0xFFF2F6F9u;
        eui_th.muted = 0xFF8E9BA5u;      eui_th.button = 0xFF343B41u;
        eui_th.button_hover = 0xFF3E464Du; eui_th.button_pressed = 0xFF2A3036u;
        eui_th.track = 0xFF3A4249u;      eui_th.disabled = 0xFF2C3237u;
        eui_th.disabled_text = 0xFF6F7C86u;
    } else {
        /* mosWM's dialogs and MOSdesk Settings */
        eui_th.background = 0xFFF3F5F6u; eui_th.surface = 0xFFFFFFFFu;
        eui_th.border = 0xFFBCC6CDu;     eui_th.text = 0xFF273640u;
        eui_th.muted = 0xFF667680u;      eui_th.button = 0xFFDCE2E6u;
        eui_th.button_hover = 0xFFD0D8DDu; eui_th.button_pressed = 0xFFC2CCD2u;
        eui_th.track = 0xFFD5DCE0u;      eui_th.disabled = 0xFFE3E7EAu;
        eui_th.disabled_text = 0xFF9AA6AEu;
    }
    eui_th.accent = a;
    eui_th.accent_hover = eui_mix(a, 0xFFFFFFFFu, 36u);
    eui_th.accent_pressed = eui_mix(a, 0xFF000000u, 46u);
    eui_th.accent_text = 0xFFFFFFFFu;
    eui_th.danger = 0xFFC8383Du;
}

/* Where MOSdesk's settings.dat keeps the username (32 bytes) and the window
 * style (0 modern, 1 Aero); mosdesk/settings.h checks these offsets. */
#define EUI_SETTINGS_USERNAME_OFFSET 828
#define EUI_SETTINGS_STYLE_OFFSET 860
static char eui_username[32];

/* Reads the accent colour and window style MOSdesk Settings saved. Every
 * layout of settings.dat starts with magic, mouse_speed, accent_color; the
 * username and style are only in files that are long enough. */
static void eui_poll_theme(void) {
    uint32_t head[3] = {0, 0, 0};
    uint8_t tail[36];
    int style = 0;
    long fd = mos_open("0:/programs/mosdesk/settings.dat", SYS_O_RDONLY);
    if (fd <= 0) return;
    long n = mos_read(fd, head, sizeof head);
    if (mos_size(fd) >= EUI_SETTINGS_STYLE_OFFSET + 4 &&
        mos_seek(fd, EUI_SETTINGS_USERNAME_OFFSET) >= 0 &&
        mos_read(fd, tail, sizeof tail) == (long)sizeof tail) {
        tail[31] = 0;
        for (int i = 0; i < 32; i++) eui_username[i] = (char)tail[i];
        style = tail[32] == 1 && !tail[33] && !tail[34] && !tail[35];
    }
    mos_close(fd);
    if (n != (long)sizeof head || head[0] != 0x4D445331u) return;
    if (head[2] == eui_settings_accent && style == eui_settings_style) return;
    eui_settings_accent = head[2];
    eui_settings_style = style;
    eui_make_theme();
    eui_dirty = 1;
}

/* ---------------------------------------------------------------------
 *  Drawing
 * --------------------------------------------------------------------- */
static void eui_clip_all(void) { eui_cx0 = 0; eui_cy0 = 0; eui_cx1 = eui_W; eui_cy1 = eui_H; }

static void eui_clip(int32_t x, int32_t y, int32_t w, int32_t h) {
    eui_clip_all();
    if (x > eui_cx0) eui_cx0 = x;
    if (y > eui_cy0) eui_cy0 = y;
    if (x + w < eui_cx1) eui_cx1 = x + w;
    if (y + h < eui_cy1) eui_cy1 = y + h;
}

static void eui_span(int32_t x0, int32_t x1, int32_t y, uint32_t c) {
    if (!eui_px || y < eui_cy0 || y >= eui_cy1) return;
    if (x0 < eui_cx0) x0 = eui_cx0;
    if (x1 > eui_cx1) x1 = eui_cx1;
    uint32_t* row = eui_px + (size_t)y * (size_t)eui_W;
    uint32_t alpha = c >> 24;
    if (alpha == 255u) { for (int32_t x = x0; x < x1; x++) row[x] = c; }
    else if (alpha) { for (int32_t x = x0; x < x1; x++) row[x] = eui_mix(row[x], c, alpha); }
}

static void eui_fillrect(int x, int y, int w, int h, uint32_t c) {
    for (int32_t py = y; py < y + h; py++) eui_span(x, x + w, py, c);
}

static void eui_drawrect(int x, int y, int w, int h, uint32_t c) {
    if (w <= 0 || h <= 0) return;
    eui_fillrect(x, y, w, 1, c); eui_fillrect(x, y + h - 1, w, 1, c);
    eui_fillrect(x, y + 1, 1, h - 2, c); eui_fillrect(x + w - 1, y + 1, 1, h - 2, c);
}

static int32_t eui_isqrt(int32_t v) {
    int32_t r = 0;
    while ((r + 1) * (r + 1) <= v) r++;
    return r;
}

/* how far row `dy` of a rounded rectangle is inset at each end */
static int32_t eui_inset(int32_t dy, int32_t h, int32_t r) {
    int32_t d = -1;
    if (dy < r) d = r - dy;
    else if (dy >= h - r) d = dy - (h - r) + 1;
    if (d < 0) return 0;
    int32_t t = r * r - (d * 2 - 1) * (d * 2 - 1) / 4;
    return r - (t > 0 ? eui_isqrt(t) : 0);
}

/* Anti-aliasing: how much of each pixel of a corner square lies inside the
 * rounded corner, 0-255, from 4x4 samples. eui_aa(r)[fy * r + fx] is the
 * pixel fx columns and fy rows in from the corner's outer edges. Built once
 * per radius; larger radii fall back to hard edges. */
#define EUI_AA_MAX 32
static uint8_t eui_aa_tab[EUI_AA_MAX + 1][EUI_AA_MAX * EUI_AA_MAX];
static uint8_t eui_aa_done[EUI_AA_MAX + 1];

static const uint8_t* eui_aa(int32_t r) {
    if (r <= 0 || r > EUI_AA_MAX) return 0;
    uint8_t* t = eui_aa_tab[r];
    if (eui_aa_done[r]) return t;
    int32_t limit = (8 * r) * (8 * r);
    for (int32_t fy = 0; fy < r; fy++)
        for (int32_t fx = 0; fx < r; fx++) {
            uint32_t inside = 0;
            for (int32_t j = 0; j < 4; j++) {
                int32_t dy = 8 * r - (8 * fy + 2 * j + 1);
                for (int32_t i = 0; i < 4; i++) {
                    int32_t dx = 8 * r - (8 * fx + 2 * i + 1);
                    if (dx * dx + dy * dy <= limit) inside++;
                }
            }
            t[fy * r + fx] = (uint8_t)(inside * 255u / 16u);
        }
    eui_aa_done[r] = 1;
    return t;
}

/* one pixel of `c` with its alpha scaled by coverage `cov` (0-255) */
static void eui_aa_pixel(int32_t x, int32_t y, uint32_t c, uint32_t cov) {
    if (!cov) return;
    if (cov < 255u)
        c = (c & 0x00FFFFFFu) | ((((c >> 24) * cov + 127u) / 255u) << 24);
    eui_span(x, x + 1, y, c);
}

/* row `dy` of a filled rounded rectangle, edge pixels anti-aliased */
static void eui_round_row(int x, int y, int w, int h, int r, int32_t dy,
                          uint32_t c) {
    int32_t fy = dy < r ? dy : (dy >= h - r ? h - 1 - dy : r);
    const uint8_t* t = fy < r ? eui_aa(r) : 0;
    if (!t) {
        int32_t in = fy < r ? eui_inset(dy, h, r) : 0;
        eui_span(x + in, x + w - in, y + dy, c);
        return;
    }
    for (int32_t fx = 0; fx < r; fx++) {
        eui_aa_pixel(x + fx, y + dy, c, t[fy * r + fx]);
        eui_aa_pixel(x + w - 1 - fx, y + dy, c, t[fy * r + fx]);
    }
    eui_span(x + r, x + w - r, y + dy, c);
}

static void eui_fillroundrect(int x, int y, int w, int h, int r, uint32_t c) {
    if (w <= 0 || h <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    for (int32_t dy = 0; dy < h; dy++) eui_round_row(x, y, w, h, r, dy, c);
}

/* a rounded rectangle `t` pixels thick, its curves anti-aliased */
static void eui_strokeround(int x, int y, int w, int h, int r, int t, uint32_t c) {
    if (w <= 0 || h <= 0 || t <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    int ri = r > t ? r - t : 0;
    const uint8_t* outer = eui_aa(r);
    const uint8_t* inner = ri ? eui_aa(ri) : 0;
    if (r > 0 && (!outer || (ri && !inner))) {
        /* too large for the tables: hard edges */
        for (int32_t dy = 0; dy < h; dy++) {
            int32_t in = eui_inset(dy, h, r);
            if (dy < t || dy >= h - t) { eui_span(x + in, x + w - in, y + dy, c); continue; }
            int32_t edge = eui_inset(dy - t, h - 2 * t, ri) + t;
            eui_span(x + in, x + edge, y + dy, c);
            eui_span(x + w - edge, x + w - in, y + dy, c);
        }
        return;
    }
    for (int32_t dy = 0; dy < h; dy++) {
        int32_t band = dy < t || dy >= h - t;
        if (dy < r || dy >= h - r) {
            int32_t fy = dy < r ? dy : h - 1 - dy;
            for (int32_t fx = 0; fx < r; fx++) {
                int32_t cov = outer[fy * r + fx];
                int32_t ix = fx - t, iy = fy - t, in = 0;
                if (ix >= 0 && iy >= 0)
                    in = (ix >= ri || iy >= ri || !inner) ? 255 : inner[iy * ri + ix];
                cov -= in;
                if (cov <= 0) continue;
                eui_aa_pixel(x + fx, y + dy, c, (uint32_t)cov);
                eui_aa_pixel(x + w - 1 - fx, y + dy, c, (uint32_t)cov);
            }
            if (band) eui_span(x + r, x + w - r, y + dy, c);
        } else if (band) {
            eui_span(x, x + w, y + dy, c);
        } else {
            eui_span(x, x + t, y + dy, c);
            eui_span(x + w - t, x + w, y + dy, c);
        }
    }
}

/* A disc centred on pixel (cx, cy), `r` pixels from the centre pixel to the
 * edge, edge pixels anti-aliased (4x4 samples). */
static void eui_fillcircle(int cx, int cy, int r, uint32_t c) {
    if (r < 0) return;
    /* radius r + 0.5 in units of 1/8 pixel, measured from the pixel centre */
    int32_t R = 8 * r + 4, R2 = R * R;
    for (int32_t dy = -r; dy <= r; dy++) {
        int32_t ay = (dy < 0 ? -dy : dy) * 8;
        int32_t solid = 0, solid_at = 0;
        for (int32_t dx = -r; dx <= 0; dx++) {
            int32_t ax = -dx * 8;
            /* fully inside when the pixel's farthest corner is */
            if ((ax + 4) * (ax + 4) + (ay + 4) * (ay + 4) <= R2) {
                solid = 1;
                solid_at = dx;
                break;
            }
            uint32_t inside = 0;
            for (int32_t j = 0; j < 4; j++) {
                int32_t sy = ay - 3 + 2 * j;
                for (int32_t i = 0; i < 4; i++) {
                    int32_t sx = ax - 3 + 2 * i;
                    if (sx * sx + sy * sy <= R2) inside++;
                }
            }
            uint32_t cov = inside * 255u / 16u;
            eui_aa_pixel(cx + dx, cy + dy, c, cov);
            if (dx) eui_aa_pixel(cx - dx, cy + dy, c, cov);
        }
        if (solid) eui_span(cx + solid_at, cx - solid_at + 1, cy + dy, c);
    }
}

static void eui_drawline(int x1, int y1, int x2, int y2, uint32_t c) {
    int dx = x2 > x1 ? x2 - x1 : x1 - x2, sx = x1 < x2 ? 1 : -1;
    int dy = y2 > y1 ? y1 - y2 : y2 - y1, sy = y1 < y2 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        eui_span(x1, x1 + 1, y1, c);
        if (x1 == x2 && y1 == y2) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x1 += sx; }
        if (e2 <= dx) { err += dx; y1 += sy; }
    }
}

static void eui_thickline(int x1, int y1, int x2, int y2, int t, uint32_t c) {
    for (int o = 0; o < t; o++) { eui_drawline(x1, y1 + o, x2, y2 + o, c); eui_drawline(x1 + o, y1, x2 + o, y2, c); }
}

/* The outline font for a text size, or NULL to use the bitmap font. Size 1
 * is 12 px regular, size 2 and up 8*size px medium, as in mosWM, unless the
 * current style (eui_tfont, eui_tpx) names a font or a pixel size. */
static ttrend_outline_font_t* eui_ttf(int size) {
    int want = size >= 2 ? 1 : 0;
    ttrend_outline_font_t* f = 0;
    if (!eui_smooth || eui_tfont == EASYUI_FONT_BITMAP) return 0;
    if (eui_tfont >= EASYUI_FONT_CUSTOM && eui_tfont - EASYUI_FONT_CUSTOM < eui_cfont_count)
        f = &eui_cfont[eui_tfont - EASYUI_FONT_CUSTOM];
    if (!f) {
        if (eui_tfont == EASYUI_FONT_REGULAR) want = 0;
        else if (eui_tfont == EASYUI_FONT_MEDIUM) want = 1;
        if (eui_ttf_ok[want]) f = &eui_ttf_font[want];
        else if (eui_ttf_ok[!want]) f = &eui_ttf_font[!want];
        else return 0;
    }
    f->pixel_size = eui_tpx ? (uint32_t)eui_tpx : size <= 1 ? 12u : 8u * (uint32_t)size;
    return f;
}

/* how much the 8x8 bitmap font is scaled up */
static int eui_bscale(int size) {
    if (eui_tpx) return eui_tpx >= 16 ? eui_tpx / 8 : 1;   /* rounded down */
    return size < 1 ? 1 : size;
}

/* height of one line of text (the box text is centred in) */
static int32_t eui_texth(int size) {
    ttrend_outline_font_t* f = eui_ttf(size);
    return f ? ttrend_outline_ascent(f) + ttrend_outline_descent(f) : 8 * eui_bscale(size);
}

/* distance from one line to the next */
static int32_t eui_lineh(int size) {
    ttrend_outline_font_t* f = eui_ttf(size);
    int32_t h = f ? ttrend_outline_line_height(f) : 10 * eui_bscale(size);
    int32_t t = eui_texth(size);
    return h > t ? h : t + 2;
}

/* width of the first n bytes of s, on one line */
static int32_t eui_linew(const char* s, int32_t n, int size) {
    ttrend_outline_font_t* f = eui_ttf(size);
    char line[260];
    if (!f) return n * 8 * eui_bscale(size);
    if (n > 259) n = 259;
    for (int32_t i = 0; i < n; i++) line[i] = s[i];
    line[n] = '\0';
    return (int32_t)ttrend_outline_text_width(line, f);
}

static int eui_textwidth(const char* s, int size) {
    int32_t best = 0;
    while (s && *s) {
        int32_t n = 0;
        while (s[n] && s[n] != '\n') n++;
        int32_t w = eui_linew(s, n, size);
        if (w > best) best = w;
        s += n;
        if (*s) s++;
    }
    return best;
}

static void eui_char(int x, int y, char ch, uint32_t c, int size) {
    unsigned char u = (unsigned char)ch;
    if (u < 32u || u > 126u) u = '?';
    const uint8_t* g = eui_font[u - 32u];
    for (int gy = 0; gy < 8; gy++) {
        uint8_t bits = g[gy];
        if (!bits) continue;
        for (int gx = 0; gx < 8; gx++)
            if (bits & (1u << gx)) eui_fillrect(x + gx * size, y + gy * size, size, size, c);
    }
}

/* one line, top-left at (x, y), clipped to the clip rectangle */
static void eui_drawline_text(int x, int y, const char* s, int32_t n, uint32_t c, int size) {
    ttrend_outline_font_t* f = eui_ttf(size);
    if (f) {
        char line[260];
        if (!eui_px || eui_cx1 <= eui_cx0 || eui_cy1 <= eui_cy0) return;
        if (n > 259) n = 259;
        for (int32_t i = 0; i < n; i++) line[i] = s[i];
        line[n] = '\0';
        /* a view of the window clipped to the clip rectangle */
        ttrend_outline_surface_t view = {
            eui_px + (size_t)eui_cy0 * (size_t)eui_W + (size_t)eui_cx0,
            (uint32_t)(eui_cx1 - eui_cx0), (uint32_t)(eui_cy1 - eui_cy0), (uint32_t)eui_W
        };
        ttrend_outline_draw_text_baseline(&view, x - eui_cx0, y + ttrend_outline_ascent(f) - eui_cy0, line, c, f);
        return;
    }
    int cx = x, scale = eui_bscale(size);
    for (int32_t i = 0; i < n; i++) {
        if (cx >= eui_cx1) break;
        if (cx + 8 * scale > eui_cx0) eui_char(cx, y, s[i], c, scale);
        cx += 8 * scale;
    }
}

static void eui_drawtext(int x, int y, const char* s, uint32_t c, int size) {
    while (s && *s) {
        int32_t n = 0;
        while (s[n] && s[n] != '\n') n++;
        eui_drawline_text(x, y, s, n, c, size);
        y += eui_lineh(size);
        s += n;
        if (*s) s++;
    }
}

/* Reads an .otf/.ttf file into the heap and parses it. Returns the file's
 * bytes (kept for the life of the font), or NULL. */
static void* eui_read_font(const char* path, ttrend_outline_font_t* font) {
    long fd = mos_open(path, SYS_O_RDONLY);
    if (fd <= 0) return 0;
    long size = mos_size(fd);
    uint8_t* data = size > 0 && size <= 8L * 1024 * 1024 ? (uint8_t*)malloc((size_t)size) : 0;
    long got = 0;
    while (data && got < size) {
        long n = mos_read(fd, data + got, (size_t)(size - got));
        if (n <= 0) break;
        got += n;
    }
    mos_close(fd);
    if (!data) return 0;
    if (got != size || ttrend_outline_font_load_memory(font, data, (size_t)size, 12u) != 0) { free(data); return 0; }
    return data;
}

/* Loads an .otf/.ttf font file for regular (bold = 0) or larger (bold = 1)
 * text. Returns 1 on success. */
static int eui_loadfont(const char* path, int bold) {
    int slot = bold ? 1 : 0;
    ttrend_outline_font_t font;
    void* data = eui_read_font(path, &font);
    if (!data) return 0;
    if (eui_ttf_data[slot]) free(eui_ttf_data[slot]);
    eui_ttf_data[slot] = data;
    eui_ttf_font[slot] = font;
    eui_ttf_ok[slot] = 1;
    eui_dirty = 1;
    return 1;
}

/* Loads one more font for setfont(). Returns its number, or 0. */
static int eui_addfont(const char* path) {
    if (eui_cfont_count >= EASYUI_MAX_FONTS) return 0;
    void* data = eui_read_font(path, &eui_cfont[eui_cfont_count]);
    if (!data) return 0;
    eui_cfont_data[eui_cfont_count] = data;
    eui_dirty = 1;
    return EASYUI_FONT_CUSTOM + eui_cfont_count++;
}

static void eui_load_default_fonts(void) {
    if (eui_fonts_tried) return;
    eui_fonts_tried = 1;
    if (!eui_ttf_ok[0]) eui_loadfont(EASYUI_FONT_REGULAR_PATH, 0);
    if (!eui_ttf_ok[1]) eui_loadfont(EASYUI_FONT_MEDIUM_PATH, 1);
}

static void eui_setsmoothtext(int on) { eui_smooth = on != 0; eui_dirty = 1; }
static int eui_smoothtext(void) { return eui_ttf(1) != 0; }

static void eui_drawpixels(int x, int y, const uint32_t* p, int w, int h) {
    if (!p) return;
    for (int py = 0; py < h; py++)
        for (int px = 0; px < w; px++) {
            uint32_t c = p[(size_t)py * (size_t)w + (size_t)px];
            eui_span(x + px, x + px + 1, y + py, c);
        }
}

/* Length of the next line of `s` that fits in `w` pixels: whole words when
 * possible, cut mid-word only when one word is wider than the box. */
static int32_t eui_fit(const char* s, int32_t w, int size) {
    int32_t len = 0, cut = -1;
    while (s[len] && s[len] != '\n') {
        if (eui_linew(s, len + 1, size) > w) break;
        if (s[len] == ' ') cut = len;
        len++;
    }
    if (s[len] && s[len] != '\n' && s[len] != ' ' && cut > 0) len = cut;
    if (len == 0 && s[0] && s[0] != '\n') len = 1;
    return len;
}

/* Draws `text` word-wrapped inside a box, vertically centred. */
static void eui_textbox(int32_t x, int32_t y, int32_t w, int32_t h, const char* text,
                        uint32_t color, int size, int align) {
    int32_t lines = 0, lh = eui_lineh(size), th = eui_texth(size), ty;
    const char* s;
    if (!text || !*text) return;
    for (s = text; *s;) {
        int32_t len = eui_fit(s, w, size);
        lines++;
        s += len;
        if (*s == ' ' || *s == '\n') s++;
    }
    ty = y + (h - ((lines - 1) * lh + th)) / 2;
    for (s = text; *s;) {
        int32_t len = eui_fit(s, w, size), shown = len;
        while (shown > 0 && s[shown - 1] == ' ') shown--;
        int32_t lw = eui_linew(s, shown, size);
        int32_t lx = align == EASYUI_ALIGN_CENTER ? x + (w - lw) / 2
                   : align == EASYUI_ALIGN_RIGHT ? x + w - lw : x;
        eui_drawline_text(lx, ty, s, shown, color, size);
        ty += lh;
        s += len;
        if (*s == ' ' || *s == '\n') s++;
    }
}

/* ---------------------------------------------------------------------
 *  Widgets
 * --------------------------------------------------------------------- */
static int eui_valid(easyui_widget id) { return id > 0 && id <= EASYUI_MAX_WIDGETS && eui_w[id].used; }

/* --- style: a widget's own setting, else the window's (eui_w[0]), else
 * the built-in look --- */
static int32_t eui_font_px(const eui_widget_t* e) {
    if (e->font_px) return e->font_px;
    if (e->size_set) return 0;
    return eui_w[0].font_px;
}

/* Text is measured and drawn in widget `e`'s font from here on; NULL goes
 * back to the window's defaults. */
static void eui_use(const eui_widget_t* e) {
    eui_tfont = e && e->font ? e->font : eui_w[0].font;
    eui_tpx = e ? eui_font_px(e) : eui_w[0].font_px;
}

static uint32_t eui_textcolor(const eui_widget_t* e, uint32_t fallback) {
    if (!e->enabled) return fallback;
    return e->text_color ? e->text_color : eui_w[0].text_color ? eui_w[0].text_color : fallback;
}

static int32_t eui_radius(const eui_widget_t* e, int32_t fallback) {
    int32_t r = e->has_radius ? e->radius : eui_w[0].has_radius ? eui_w[0].radius : fallback;
    return r < 0 ? 0 : r;
}

static int32_t eui_pad(const eui_widget_t* e, int32_t fallback) {
    return e->has_padding ? e->padding : eui_w[0].has_padding ? eui_w[0].padding : fallback;
}

/* the widget's own border, if it has one (or the window default) */
static int eui_border(const eui_widget_t* e, uint32_t* color, int32_t* width) {
    const eui_widget_t* from = e->has_border ? e : eui_w[0].has_border ? &eui_w[0] : 0;
    if (!from || from->border_width <= 0) return 0;
    *color = from->border_color ? from->border_color : eui_th.border;
    *width = from->border_width;
    return 1;
}

/* what a text input shows: its text, or stars for a password */
static int32_t eui_shown(const eui_widget_t* e, char* out) {
    int32_t n = 0;
    for (; e->text[n]; n++) out[n] = e->password ? '*' : e->text[n];
    out[n] = '\0';
    return n;
}

static void eui_copy(char* dst, const char* src, int size) {
    int i = 0;
    for (; src && src[i] && i + 1 < size; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static easyui_widget eui_new(int type, int x, int y, int w, int h, const char* text) {
    for (int id = 1; id <= EASYUI_MAX_WIDGETS; id++) {
        if (eui_w[id].used) continue;
        eui_widget_t* e = &eui_w[id];
        memset(e, 0, sizeof *e);
        e->used = 1; e->type = (uint8_t)type; e->visible = 1; e->enabled = 1;
        e->x = x; e->y = y; e->w = w < 1 ? 1 : w; e->h = h < 1 ? 1 : h;
        e->size = 1; e->item_hover = -1; e->max = 100; e->step = 1; e->maxlength = EASYUI_TEXT_MAX - 1;
        e->anchor = EASYUI_ANCHOR_LEFT | EASYUI_ANCHOR_TOP;
        e->align = type == EASYUI_BUTTON ? EASYUI_ALIGN_CENTER : EASYUI_ALIGN_LEFT;
        eui_copy(e->text, text, EASYUI_TEXT_MAX);
        e->margin_right = eui_W - (x + w);
        e->margin_bottom = eui_H - (y + h);
        eui_dirty = 1;
        return id;
    }
    return 0;
}

static int eui_focusable(int id) {
    if (!eui_valid(id) || !eui_w[id].visible || !eui_w[id].enabled) return 0;
    int t = eui_w[id].type;
    return t == EASYUI_BUTTON || t == EASYUI_SLIDER || t == EASYUI_CHECKBOX || t == EASYUI_RADIO ||
           t == EASYUI_TOGGLE || t == EASYUI_TEXTINPUT || t == EASYUI_DROPDOWN || t == EASYUI_LISTBOX;
}

static void eui_fire(int id, int event, int value) {
    if (id < 0 || id > EASYUI_MAX_WIDGETS || (id && !eui_w[id].used)) return;
    easyui_callback fn = eui_w[id].cb[event];
    if (!fn) return;
    if (event == EASYUI_ONCE) eui_w[id].cb[event] = 0;
    int prev = eui_cur;
    eui_cur = id;
    fn(value);
    eui_cur = prev;
    eui_dirty = 1;
}

static void eui_set_focus(int id) {
    if (id && !eui_focusable(id)) id = 0;
    if (id == eui_focus) return;
    int old = eui_focus;
    eui_focus = id;
    if (eui_open && eui_open != id) eui_open = 0;
    eui_caret_on = 1; eui_caret_time = mos_uptime();
    eui_dirty = 1;
    if (old) eui_fire(old, EASYUI_ONBLUR, 0);
    if (id) eui_fire(id, EASYUI_ONFOCUS, 0);
}

static int32_t eui_clampv(eui_widget_t* e, int32_t v) {
    int32_t lo = e->min < e->max ? e->min : e->max, hi = e->min < e->max ? e->max : e->min;
    return v < lo ? lo : v > hi ? hi : v;
}

static void eui_change(int id, int32_t v) {
    eui_widget_t* e = &eui_w[id];
    if (e->type == EASYUI_SLIDER || e->type == EASYUI_PROGRESS) v = eui_clampv(e, v);
    if (v == e->value) return;
    e->value = v;
    eui_dirty = 1;
    eui_fire(id, EASYUI_ONCHANGE, v);
}

static void eui_select_radio(int id) {
    eui_widget_t* e = &eui_w[id];
    for (int i = 1; i <= EASYUI_MAX_WIDGETS; i++) {
        if (i == id || !eui_w[i].used || eui_w[i].type != EASYUI_RADIO || eui_w[i].group != e->group) continue;
        if (eui_w[i].value) { eui_w[i].value = 0; eui_fire(i, EASYUI_ONCHANGE, 0); }
    }
    eui_change(id, 1);
}

/* one click (mouse or keyboard) on a widget */
static void eui_activate(int id) {
    eui_widget_t* e = &eui_w[id];
    if (e->type == EASYUI_CHECKBOX || e->type == EASYUI_TOGGLE) eui_change(id, !e->value);
    else if (e->type == EASYUI_RADIO) eui_select_radio(id);
    if (!eui_valid(id)) return;
    eui_fire(id, EASYUI_ONCLICK, 0);
    eui_fire(id, EASYUI_ONCE, 0);
}

static int32_t eui_row_h(eui_widget_t* e) {
    int font = eui_tfont, px = eui_tpx;
    eui_use(e);
    int32_t h = eui_texth(e->size) + 12;
    eui_tfont = font; eui_tpx = px;
    return h;
}

static int32_t eui_list_rows(eui_widget_t* e) {
    int32_t rows = (e->h - 4) / eui_row_h(e);
    return rows < 1 ? 1 : rows;
}

static void eui_list_scroll_to(eui_widget_t* e, int32_t index) {
    int32_t rows = eui_list_rows(e);
    if (index < e->scroll) e->scroll = index;
    if (index >= e->scroll + rows) e->scroll = index - rows + 1;
    if (e->scroll > e->count - rows) e->scroll = e->count - rows;
    if (e->scroll < 0) e->scroll = 0;
}

/* the open dropdown's list, below (or above) the dropdown */
static int eui_dd_rect(int32_t* x, int32_t* y, int32_t* w, int32_t* h) {
    if (!eui_valid(eui_open)) return 0;
    eui_widget_t* e = &eui_w[eui_open];
    int32_t rows = e->count < 8 ? e->count : 8;
    if (rows < 1) rows = 1;
    *x = e->x; *w = e->w; *h = rows * eui_row_h(e) + 4;
    *y = e->y + e->h + 2;
    if (*y + *h > eui_H && e->y - *h - 2 >= 0) *y = e->y - *h - 2;
    return 1;
}

static int eui_hit(int32_t x, int32_t y) {
    for (int id = EASYUI_MAX_WIDGETS; id >= 1; id--) {
        eui_widget_t* e = &eui_w[id];
        if (!e->used || !e->visible || e->type == EASYUI_SEPARATOR) continue;
        if (x >= e->x && y >= e->y && x < e->x + e->w && y < e->y + e->h) return id;
    }
    return 0;
}

static int32_t eui_slider_vertical(eui_widget_t* e) { return e->h > e->w; }

static void eui_slider_drag(int id, int32_t mx, int32_t my) {
    eui_widget_t* e = &eui_w[id];
    int vertical = eui_slider_vertical(e);
    int32_t len = vertical ? e->h : e->w, thick = vertical ? e->w : e->h;
    int32_t r = thick / 2 < 9 ? thick / 2 : 9;
    int32_t a = r, b = len - r;
    int32_t pos = vertical ? (e->y + e->h - my) : (mx - e->x);
    if (b <= a) return;
    if (pos < a) pos = a;
    if (pos > b) pos = b;
    int32_t span = e->max - e->min;
    int32_t v = e->min + (int32_t)(((int64_t)(pos - a) * span * 2 + (b - a)) / (2 * (int64_t)(b - a)));
    if (e->step > 1) v = e->min + (v - e->min + e->step / 2) / e->step * e->step;
    eui_change(id, v);
}

static void eui_press(int id) {
    if (eui_open) {
        int32_t x, y, w, h;
        eui_dd_rect(&x, &y, &w, &h);
        int dd = eui_open;
        if (eui_mx >= x && eui_mx < x + w && eui_my >= y && eui_my < y + h) {
            eui_widget_t* e = &eui_w[dd];
            int32_t row = (eui_my - y - 2) / eui_row_h(e) + e->scroll;
            eui_open = 0;
            if (row >= 0 && row < e->count) eui_change(dd, row);
            eui_dirty = 1;
            return;
        }
        eui_open = 0;
        eui_dirty = 1;
        if (id == dd) { eui_pressed = 0; return; }
    }
    if (!id || !eui_w[id].enabled) { eui_set_focus(0); return; }
    eui_widget_t* e = &eui_w[id];
    eui_pressed = id;
    eui_set_focus(eui_focusable(id) ? id : 0);
    if (!eui_valid(id)) return;
    eui_fire(id, EASYUI_ONPRESS, 1);
    if (!eui_valid(id)) return;
    if (e->type == EASYUI_SLIDER) eui_slider_drag(id, eui_mx, eui_my);
    else if (e->type == EASYUI_DROPDOWN) { eui_open = id; e->scroll = e->value > 7 ? e->value - 7 : 0; }
    else if (e->type == EASYUI_TEXTINPUT) {
        char shown[EASYUI_TEXT_MAX];
        int32_t len = eui_shown(e, shown), target = eui_mx - e->x - eui_pad(e, 8) + e->scroll, best = 0, dist = -1;
        eui_use(e);
        for (int32_t i = 0; i <= len; i++) {
            int32_t d = eui_linew(shown, i, e->size) - target;
            if (d < 0) d = -d;
            if (dist < 0 || d < dist) { dist = d; best = i; }
        }
        eui_use(0);
        e->cursor = best;
    } else if (e->type == EASYUI_LISTBOX) {
        int32_t row = (eui_my - e->y - 2) / eui_row_h(e) + e->scroll;
        if (row >= 0 && row < e->count) {
            long now = mos_uptime();
            int dbl = row == eui_last_click_item && now - eui_last_click_time < 450;
            eui_last_click_item = row; eui_last_click_time = now;
            eui_change(id, row);
            if (dbl && eui_valid(id)) eui_fire(id, EASYUI_ONSUBMIT, row);
        }
    }
    eui_dirty = 1;
}

static void eui_release(int id) {
    int p = eui_pressed;
    eui_pressed = 0;
    if (!eui_valid(p)) return;
    eui_dirty = 1;
    eui_fire(p, EASYUI_ONRELEASE, 0);
    if (eui_valid(p) && id == p && eui_w[p].enabled) eui_activate(p);
}

static int eui_pick_active;
static void eui_pick_mouse(int32_t x, int32_t y, int pressed);
static void eui_pick_wheel(int32_t delta);
static void eui_pick_key(int key, int mods);

static void eui_mouse(int32_t x, int32_t y, uint32_t buttons) {
    int down = (buttons & 1u) != 0, was = (eui_buttons & 1u) != 0;
    eui_mx = x; eui_my = y; eui_buttons = buttons;
    if (eui_pick_active && !eui_msg_active) { eui_pick_mouse(x, y, down && !was); return; }
    if (eui_msg_active) {
        int32_t bw = 90, bh = 30, mw = eui_W - 40 < 380 ? eui_W - 40 : 380, mh = 170;
        int32_t bx = (eui_W + mw) / 2 - bw - 16, by = (eui_H + mh) / 2 - bh - 16;
        int over = x >= bx && y >= by && x < bx + bw && y < by + bh;
        if (over != eui_msg_hover) { eui_msg_hover = over; eui_dirty = 1; }
        if (!down && was && over) { eui_msg_active = 0; eui_dirty = 1; }
        return;
    }
    if (eui_open) {
        int32_t dx, dy, dw, dh, row = -1;
        eui_dd_rect(&dx, &dy, &dw, &dh);
        if (x >= dx && x < dx + dw && y >= dy && y < dy + dh) row = (y - dy - 2) / eui_row_h(&eui_w[eui_open]) + eui_w[eui_open].scroll;
        if (row != eui_w[eui_open].item_hover) { eui_w[eui_open].item_hover = row; eui_dirty = 1; }
    }
    int hit = eui_hit(x, y);
    if (eui_pressed && eui_w[eui_pressed].type == EASYUI_SLIDER) hit = eui_hover;   /* keep the hover while dragging */
    if (hit != eui_hover) {
        int old = eui_hover;
        eui_hover = hit;
        if (eui_valid(old)) { eui_w[old].hovered = 0; eui_w[old].item_hover = -1; eui_fire(old, EASYUI_ONLEAVE, 0); }
        if (eui_valid(hit)) { eui_w[hit].hovered = 1; eui_fire(hit, EASYUI_ONHOVER, 0); }
        eui_dirty = 1;
    }
    if (eui_valid(hit) && eui_w[hit].type == EASYUI_LISTBOX) {
        eui_widget_t* e = &eui_w[hit];
        int32_t row = (y - e->y - 2) / eui_row_h(e) + e->scroll;
        if (row >= e->count) row = -1;
        if (row != e->item_hover) { e->item_hover = row; eui_dirty = 1; }
    }
    if (down && !was) eui_press(hit);
    else if (!down && was) eui_release(eui_hit(x, y));
    else if (down && eui_valid(eui_pressed) && eui_w[eui_pressed].type == EASYUI_SLIDER) eui_slider_drag(eui_pressed, x, y);
}

static void eui_wheel(int32_t delta) {
    if (eui_pick_active) { if (!eui_msg_active) eui_pick_wheel(delta); return; }
    int id = eui_open ? eui_open : eui_hover;
    if (!eui_valid(id)) return;
    eui_widget_t* e = &eui_w[id];
    if (e->type == EASYUI_CANVAS) { eui_fire(id, EASYUI_ONWHEEL, delta > 0 ? 1 : -1); return; }
    int steps = delta > 0 ? 1 : -1;
    if (e->type == EASYUI_LISTBOX || (e->type == EASYUI_DROPDOWN && eui_open)) {
        int32_t rows = e->type == EASYUI_LISTBOX ? eui_list_rows(e) : 8;
        e->scroll -= steps;
        if (e->scroll > e->count - rows) e->scroll = e->count - rows;
        if (e->scroll < 0) e->scroll = 0;
        eui_dirty = 1;
    } else if (e->type == EASYUI_SLIDER && e->enabled) eui_change(id, e->value + steps * e->step);
}

static char eui_ascii(int key, int shift) {
    static const char lower[] = "abcdefghijklmnopqrstuvwxyz1234567890";
    static const char upper[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()";
    static const char sym_lo[] = "-=[]\\#;'`,./";
    static const char sym_up[] = "_+{}|~:\"~<>?";
    if (key >= 4 && key <= 39) return shift ? upper[key - 4] : lower[key - 4];
    if (key == EASYUI_KEY_SPACE) return ' ';
    if (key >= 45 && key <= 56 && key != 50) return shift ? sym_up[key - 45] : sym_lo[key - 45];
    return 0;
}

static void eui_text_key(int id, int key, int mods) {
    eui_widget_t* e = &eui_w[id];
    int32_t len = (int32_t)strlen(e->text);
    int changed = 0;
    if (e->cursor > len) e->cursor = len;
    char c = (mods & SYSCALL_INPUT_MOD_CTRL) ? 0 : eui_ascii(key, (mods & SYSCALL_INPUT_MOD_SHIFT) != 0);
    if (c) {
        if (len < e->maxlength && len < EASYUI_TEXT_MAX - 1) {
            memmove(e->text + e->cursor + 1, e->text + e->cursor, (size_t)(len - e->cursor + 1));
            e->text[e->cursor++] = c;
            changed = 1;
        }
    } else if (key == EASYUI_KEY_BACKSPACE && e->cursor > 0) {
        memmove(e->text + e->cursor - 1, e->text + e->cursor, (size_t)(len - e->cursor + 1));
        e->cursor--; changed = 1;
    } else if (key == EASYUI_KEY_DELETE && e->cursor < len) {
        memmove(e->text + e->cursor, e->text + e->cursor + 1, (size_t)(len - e->cursor));
        changed = 1;
    } else if (key == EASYUI_KEY_LEFT && e->cursor > 0) e->cursor--;
    else if (key == EASYUI_KEY_RIGHT && e->cursor < len) e->cursor++;
    else if (key == EASYUI_KEY_HOME) e->cursor = 0;
    else if (key == EASYUI_KEY_END) e->cursor = len;
    else if (key == EASYUI_KEY_ENTER) { eui_fire(id, EASYUI_ONSUBMIT, len); return; }
    else if (key == EASYUI_KEY_ESCAPE) { eui_set_focus(0); return; }
    eui_caret_on = 1; eui_caret_time = mos_uptime();
    eui_dirty = 1;
    if (changed) eui_fire(id, EASYUI_ONCHANGE, (int)strlen(e->text));
}

static void eui_focus_next(int dir) {
    int id = eui_focus;
    for (int i = 0; i < EASYUI_MAX_WIDGETS; i++) {
        id += dir;
        if (id > EASYUI_MAX_WIDGETS) id = 1;
        if (id < 1) id = EASYUI_MAX_WIDGETS;
        if (eui_focusable(id)) { eui_set_focus(id); return; }
    }
}

static void eui_key(int key, int mods) {
    if (eui_msg_active) {
        if (key == EASYUI_KEY_ENTER || key == EASYUI_KEY_ESCAPE || key == EASYUI_KEY_SPACE) { eui_msg_active = 0; eui_dirty = 1; }
        return;
    }
    if (eui_pick_active) { eui_pick_key(key, mods); return; }
    int id = eui_focus;
    int value = key | (mods << 8);
    if (key == EASYUI_KEY_TAB) { eui_focus_next((mods & SYSCALL_INPUT_MOD_SHIFT) ? -1 : 1); return; }
    if (eui_valid(id)) {
        eui_widget_t* e = &eui_w[id];
        int t = e->type;
        int back = key == EASYUI_KEY_LEFT || key == EASYUI_KEY_DOWN;
        int fwd = key == EASYUI_KEY_RIGHT || key == EASYUI_KEY_UP;
        if (t == EASYUI_TEXTINPUT) eui_text_key(id, key, mods);
        else if ((t == EASYUI_BUTTON || t == EASYUI_CHECKBOX || t == EASYUI_TOGGLE || t == EASYUI_RADIO) &&
                 (key == EASYUI_KEY_SPACE || key == EASYUI_KEY_ENTER)) {
            eui_fire(id, EASYUI_ONPRESS, 1);
            if (eui_valid(id)) eui_fire(id, EASYUI_ONRELEASE, 0);
            if (eui_valid(id)) eui_activate(id);
        } else if (t == EASYUI_SLIDER && (back || fwd)) eui_change(id, e->value + (fwd ? e->step : -e->step));
        else if (t == EASYUI_SLIDER && key == EASYUI_KEY_HOME) eui_change(id, e->min);
        else if (t == EASYUI_SLIDER && key == EASYUI_KEY_END) eui_change(id, e->max);
        else if (t == EASYUI_RADIO && (back || fwd)) {
            /* move to the previous/next radio button of the same group */
            int n = id;
            for (int i = 0; i < EASYUI_MAX_WIDGETS; i++) {
                n += fwd ? 1 : -1;
                if (n > EASYUI_MAX_WIDGETS) n = 1;
                if (n < 1) n = EASYUI_MAX_WIDGETS;
                if (eui_focusable(n) && eui_w[n].type == EASYUI_RADIO && eui_w[n].group == e->group) break;
            }
            eui_set_focus(n);
            eui_select_radio(n);
        } else if ((t == EASYUI_LISTBOX || t == EASYUI_DROPDOWN) && (key == EASYUI_KEY_UP || key == EASYUI_KEY_DOWN)) {
            int32_t v = e->value + (key == EASYUI_KEY_DOWN ? 1 : -1);
            if (v >= 0 && v < e->count) { eui_change(id, v); if (eui_valid(id)) eui_list_scroll_to(e, v); }
        } else if (t == EASYUI_DROPDOWN && (key == EASYUI_KEY_SPACE || key == EASYUI_KEY_ENTER)) {
            eui_open = eui_open ? 0 : id; eui_dirty = 1;
        } else if (t == EASYUI_LISTBOX && key == EASYUI_KEY_ENTER && e->value >= 0 && e->value < e->count)
            eui_fire(id, EASYUI_ONSUBMIT, e->value);
        else if (key == EASYUI_KEY_ESCAPE) { if (eui_open) { eui_open = 0; eui_dirty = 1; } else eui_set_focus(0); }
        if (eui_valid(id)) eui_fire(id, EASYUI_ONKEY, value);
    }
    eui_fire(EASYUI_WINDOW, EASYUI_ONKEY, value);
}

static void eui_apply_anchors(void) {
    for (int id = 1; id <= EASYUI_MAX_WIDGETS; id++) {
        eui_widget_t* e = &eui_w[id];
        if (!e->used) continue;
        if (e->anchor & EASYUI_ANCHOR_RIGHT) {
            if (e->anchor & EASYUI_ANCHOR_LEFT) { e->w = eui_W - e->x - e->margin_right; if (e->w < 1) e->w = 1; }
            else e->x = eui_W - e->w - e->margin_right;
        }
        if (e->anchor & EASYUI_ANCHOR_BOTTOM) {
            if (e->anchor & EASYUI_ANCHOR_TOP) { e->h = eui_H - e->y - e->margin_bottom; if (e->h < 1) e->h = 1; }
            else e->y = eui_H - e->h - e->margin_bottom;
        }
    }
}

static int eui_resize(int32_t w, int32_t h) {
    if (w < 1 || h < 1) return 0;
    uint32_t* p = (uint32_t*)realloc(eui_px, (size_t)w * (size_t)h * 4u);
    if (!p) return 0;
    eui_px = p; eui_W = w; eui_H = h;
    eui_apply_anchors();
    eui_dirty = 1;
    return 1;
}

/* ---------------------------------------------------------------------
 *  Painting
 * --------------------------------------------------------------------- */
static uint32_t eui_accent_of(eui_widget_t* e) { return e->color ? (0xFF000000u | e->color) : eui_th.accent; }

/* whether dark text reads better than white on `c` */
static int eui_light(uint32_t c) {
    return (((c >> 16) & 255u) * 299u + ((c >> 8) & 255u) * 587u + (c & 255u) * 114u) / 1000u > 150u;
}

/* A rounded rectangle whose upper half fades from t0 to t1 and lower half
 * from b0 to b1: the two-tone glass of Aero buttons and bars. */
static void eui_vgrad2(int x, int y, int w, int h, int r,
                       uint32_t t0, uint32_t t1, uint32_t b0, uint32_t b1) {
    if (w <= 0 || h <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    int32_t half = h / 2 > 0 ? h / 2 : 1, rest = h - half > 1 ? h - half - 1 : 1;
    for (int32_t dy = 0; dy < h; dy++) {
        uint32_t c = dy < half ? eui_mix(t0, t1, (uint32_t)(dy * 255 / half))
                               : eui_mix(b0, b1, (uint32_t)((dy - half) * 255 / rest));
        eui_round_row(x, y, w, h, r, dy, c);
    }
}

/* A Windows 7 push button: grey glass, light blue when hovered, deeper blue
 * while pressed. A non-zero `base` makes glass of that colour instead. */
static void eui_aero_button(int x, int y, int w, int h, int r, int hover,
                            int pressed, int emphasis, uint32_t base) {
    uint32_t edge, t0, t1, b0, b1;
    if (base) {
        uint32_t b = pressed ? eui_mix(base, 0xFF000000u, 40u)
                   : hover ? eui_mix(base, 0xFFFFFFFFu, 40u) : base;
        edge = eui_mix(base, 0xFF000000u, 100u);
        t0 = eui_mix(b, 0xFFFFFFFFu, 150u); t1 = eui_mix(b, 0xFFFFFFFFu, 100u);
        b0 = b; b1 = eui_mix(b, 0xFFFFFFFFu, 50u);
    } else if (pressed) {
        edge = 0xFF2C628Bu; t0 = 0xFFE5F4FCu; t1 = 0xFFC4E5F6u; b0 = 0xFF98D1EFu; b1 = 0xFF68B3DBu;
    } else if (hover) {
        edge = 0xFF3C7FB1u; t0 = 0xFFEAF6FDu; t1 = 0xFFD9F0FCu; b0 = 0xFFBEE6FDu; b1 = 0xFFA7D9F5u;
    } else {
        edge = emphasis ? 0xFF3C7FB1u : 0xFF707070u;
        t0 = 0xFFF2F2F2u; t1 = 0xFFEBEBEBu; b0 = 0xFFDDDDDDu; b1 = 0xFFCFCFCFu;
    }
    eui_fillroundrect(x, y, w, h, r, edge);
    eui_vgrad2(x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 1 : 0, t0, t1, b0, b1);
    eui_strokeround(x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 1 : 0, 1,
                    emphasis && !hover && !pressed && !base ? 0xA048C6F4u : 0x70FFFFFFu);
}

static void eui_focus_ring(eui_widget_t* e, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r) {
    (void)e;
    if (eui_aero) {
        eui_strokeround(x - 2, y - 2, w + 4, h + 4, r + 2, 1, 0xC03399FFu);
        return;
    }
    eui_strokeround(x - 3, y - 3, w + 6, h + 6, r + 3, 2, (eui_th.accent & 0x00FFFFFFu) | 0xA0000000u);
}

static void eui_check(int32_t x, int32_t y, int32_t s, uint32_t c) {
    int t = s / 8 > 1 ? s / 8 : 1;
    eui_thickline(x + s * 2 / 9, y + s / 2, x + s * 4 / 10, y + s * 7 / 10, t, c);
    eui_thickline(x + s * 4 / 10, y + s * 7 / 10, x + s * 3 / 4, y + s * 3 / 10, t, c);
}

/* The Aero list row highlight: blue for the selection (grey when the list
 * does not have the focus), pale blue for the row under the pointer. */
static void eui_aero_row(int x, int y, int w, int h, int selected, int focused) {
    if (selected && focused) {
        eui_fillroundrect(x, y, w, h, 3, 0xFF7DA2CEu);
        eui_vgrad2(x + 1, y + 1, w - 2, h - 2, 2, 0xFFEBF4FDu, 0xFFDCEBFCu, 0xFFD3E6FBu, 0xFFC1DBFCu);
    } else if (selected) {
        eui_fillroundrect(x, y, w, h, 3, 0xFFD9D9D9u);
        eui_vgrad2(x + 1, y + 1, w - 2, h - 2, 2, 0xFFF8F8F8u, 0xFFF0F0F0u, 0xFFEBEBEBu, 0xFFE5E5E5u);
    } else {
        eui_fillroundrect(x, y, w, h, 3, 0xFFB8D6FBu);
        eui_vgrad2(x + 1, y + 1, w - 2, h - 2, 2, 0xFFFAFBFDu, 0xFFF2F8FEu, 0xFFEBF3FDu, 0xFFE6F0FAu);
    }
}

static void eui_paint_widget(int id) {
    eui_widget_t* e = &eui_w[id];
    int32_t x = e->x, y = e->y, w = e->w, h = e->h, sz = e->size;
    uint32_t text = eui_textcolor(e, e->enabled ? eui_th.text : eui_th.disabled_text);
    uint32_t acc = eui_accent_of(e);
    uint32_t bg = e->background;          /* 0: the theme's colour */
    uint32_t bcol = 0;
    int32_t bw = 0, radius = 0;           /* outline for setborder() */
    int border = eui_border(e, &bcol, &bw);
    int focus = id == eui_focus, hover = e->hovered && e->enabled, pressed = id == eui_pressed && hover;
    int aero = eui_aero;
    eui_use(e);
    int32_t th = eui_texth(sz);
    switch (e->type) {
    case EASYUI_BUTTON: {
        uint32_t fill = 0, fg = eui_th.text;
        int32_t r = eui_radius(e, aero ? 3 : h / 2 < 7 ? h / 2 : 7), pad = eui_pad(e, 8);
        radius = r;
        if (aero && e->enabled && (e->style != EASYUI_FLAT || hover || bg)) {
            uint32_t base = bg ? bg : e->style == EASYUI_DANGER ? eui_th.danger
                          : e->style == EASYUI_PRIMARY && e->color ? acc : 0;
            if (e->style == EASYUI_FLAT && !bg) base = 0;
            eui_aero_button(x, y, w, h, r, hover, pressed, e->style == EASYUI_PRIMARY, base);
            fg = base && !eui_light(base) ? 0xFFFFFFFFu : 0xFF000000u;
            if (e->style == EASYUI_FLAT && !bg) fg = acc;
        } else if (aero && !e->enabled) {
            eui_fillroundrect(x, y, w, h, r, 0xFFADB2B5u);
            eui_fillroundrect(x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 1 : 0, 0xFFF4F4F4u);
            fg = eui_th.disabled_text;
        } else if (aero) {
            fg = acc;                      /* flat, not hovered */
        } else {
            if (!e->enabled) { fill = eui_th.disabled; fg = eui_th.disabled_text; }
            else if (bg) {
                fill = pressed ? eui_mix(bg, 0xFF000000u, 46u) : hover ? eui_mix(bg, 0xFFFFFFFFu, 36u) : bg;
                fg = eui_light(bg) ? 0xFF1B262Eu : 0xFFFFFFFFu;
            } else if (e->style == EASYUI_SECONDARY) fill = pressed ? eui_th.button_pressed : hover ? eui_th.button_hover : eui_th.button;
            else if (e->style == EASYUI_FLAT) { fill = pressed ? eui_th.button_pressed : hover ? eui_th.button_hover : 0; fg = acc; }
            else {
                uint32_t base = e->style == EASYUI_DANGER ? eui_th.danger : acc;
                fill = pressed ? eui_mix(base, 0xFF000000u, 46u) : hover ? eui_mix(base, 0xFFFFFFFFu, 36u) : base;
                fg = eui_th.accent_text;
            }
            if (fill) eui_fillroundrect(x, y, w, h, r, fill);
        }
        fg = eui_textcolor(e, fg);
        if (focus) eui_focus_ring(e, x, y, w, h, r);
        eui_clip(x + 2, y, w - 4, h);
        eui_textbox(x + pad, y + (aero && pressed ? 1 : 0), w - 2 * pad, h, e->text, fg, sz, e->align);
        break;
    }
    case EASYUI_LABEL: {
        int32_t r = eui_radius(e, 0), pad = eui_pad(e, bg || border ? 6 : 0);
        uint32_t color = e->text_color || !e->color || !e->enabled ? text : (0xFF000000u | e->color);
        radius = r;
        if (bg) eui_fillroundrect(x, y, w, h, r, bg);
        eui_clip(x, y, w, h);
        eui_textbox(x + pad, y, w - 2 * pad, h, e->text, color, sz, e->align);
        break;
    }
    case EASYUI_SLIDER: {
        int vertical = eui_slider_vertical(e);
        int32_t len = vertical ? h : w, thick = vertical ? w : h;
        int32_t r = thick / 2 < 9 ? thick / 2 : 9;
        int32_t span = e->max - e->min;
        int32_t pos = r + (span ? (int32_t)((int64_t)(e->value - e->min) * (len - 2 * r) / span) : 0);
        uint32_t fill = e->enabled ? acc : eui_th.disabled_text;
        uint32_t track = bg ? bg : eui_th.track;
        int lit = hover || focus || id == eui_pressed;
        if (aero) {
            /* a sunken groove and a glass thumb */
            int32_t tl = thick - 4 < 22 ? thick - 4 : 22;
            if (tl < 8) tl = 8;
            if (vertical) {
                int32_t cx = x + w / 2, ky = y + h - pos;
                eui_fillrect(cx - 2, y + r, 4, h - 2 * r, 0xFFB0B4B8u);
                eui_fillrect(cx - 1, y + r + 1, 2, h - 2 * r - 2, bg ? bg : 0xFFE7EAEAu);
                eui_fillrect(cx - 1, ky, 2, y + h - r - 1 - ky, fill);
                eui_aero_button(cx - tl / 2, ky - 5, tl, 11, 2, lit, id == eui_pressed, 0, 0);
            } else {
                int32_t cy = y + h / 2, kx = x + pos;
                eui_fillrect(x + r, cy - 2, w - 2 * r, 4, 0xFFB0B4B8u);
                eui_fillrect(x + r + 1, cy - 1, w - 2 * r - 2, 2, bg ? bg : 0xFFE7EAEAu);
                eui_fillrect(x + r + 1, cy - 1, kx - x - r - 1, 2, fill);
                eui_aero_button(kx - 5, cy - tl / 2, 11, tl, 2, lit, id == eui_pressed, 0, 0);
            }
        } else if (vertical) {
            int32_t cx = x + w / 2, ky = y + h - pos;
            eui_fillroundrect(cx - 2, y + r - 2, 4, h - 2 * r + 4, 2, track);
            eui_fillroundrect(cx - 2, ky, 4, y + h - r + 2 - ky, 2, fill);
            if (lit) eui_fillcircle(cx, ky, r + 3, (fill & 0x00FFFFFFu) | 0x40000000u);
            eui_fillcircle(cx, ky, r, fill);
            eui_fillcircle(cx, ky, r - 3, eui_th.surface);
        } else {
            int32_t cy = y + h / 2, kx = x + pos;
            eui_fillroundrect(x + r - 2, cy - 2, w - 2 * r + 4, 4, 2, track);
            eui_fillroundrect(x + r - 2, cy - 2, kx - x - r + 2, 4, 2, fill);
            if (lit) eui_fillcircle(kx, cy, r + 3, (fill & 0x00FFFFFFu) | 0x40000000u);
            eui_fillcircle(kx, cy, r, fill);
            eui_fillcircle(kx, cy, r - 3, eui_th.surface);
        }
        radius = eui_radius(e, 0);
        break;
    }
    case EASYUI_CHECKBOX:
    case EASYUI_RADIO:
    case EASYUI_TOGGLE: {
        int32_t box = eui_font_px(e) ? (th + 4 > 12 ? th + 4 : 12) : 10 + 8 * sz;
        int32_t bx = x, by, lx;
        uint32_t on = e->enabled ? acc : eui_th.disabled_text;
        radius = eui_radius(e, 0);
        if (bg) { eui_fillroundrect(x, y, w, h, radius, bg); bx += eui_pad(e, 6); }
        else bx += eui_pad(e, 0);
        if (aero && e->type != EASYUI_TOGGLE) box = box * 3 / 4 + 1;
        by = y + (h - box) / 2;
        if (e->type == EASYUI_TOGGLE) {
            int32_t tw = box * 2 - 2;
            if (aero) {
                uint32_t a = e->value ? on : 0xFFE6E6E6u, b = e->value ? eui_mix(on, 0xFF000000u, 40u) : 0xFFCFCFCFu;
                eui_fillroundrect(bx, by, tw, box, box / 2, e->value ? eui_mix(on, 0xFF000000u, 90u) : hover ? 0xFF3C7FB1u : 0xFF8E8F8Fu);
                eui_vgrad2(bx + 1, by + 1, tw - 2, box - 2, box / 2 - 1, eui_mix(a, 0xFFFFFFFFu, 120u), a, b, a);
                eui_aero_button(e->value ? bx + tw - box + 1 : bx + 1, by + 1, box - 2, box - 2, box / 2 - 1, hover, 0, 0, 0);
            } else {
                eui_fillroundrect(bx, by, tw, box, box / 2, e->value ? on : (hover ? eui_th.button_pressed : eui_th.track));
                eui_fillcircle(e->value ? bx + tw - box / 2 - 1 : bx + box / 2, by + box / 2, box / 2 - 3, 0xFFFFFFFFu);
            }
            if (focus) eui_focus_ring(e, bx, by, tw, box, box / 2);
            lx = bx + tw + 10;
        } else if (e->type == EASYUI_RADIO) {
            int32_t cx = bx + box / 2, cy = by + box / 2;
            if (aero) {
                uint32_t dot = e->color ? acc : 0xFF1C6EA8u;
                eui_fillcircle(cx, cy, box / 2, hover ? 0xFF3C7FB1u : 0xFF8E8F8Fu);
                eui_fillcircle(cx, cy, box / 2 - 1, hover ? 0xFFDEF9FAu : 0xFFE6E6E6u);
                eui_fillcircle(cx, cy - 1, box / 2 - 3, 0xFFFFFFFFu);
                if (e->value) {
                    eui_fillcircle(cx, cy, box / 2 - 3, e->enabled ? dot : eui_th.disabled_text);
                    eui_fillcircle(cx - 1, cy - 1, (box / 2 - 3) / 2, 0x90FFFFFFu);
                }
            } else {
                eui_fillcircle(cx, cy, box / 2, e->value ? on : (hover ? acc : eui_th.border));
                eui_fillcircle(cx, cy, box / 2 - 2, eui_th.surface);
                if (e->value) eui_fillcircle(cx, cy, box / 2 - 5, on);
            }
            if (focus) eui_focus_ring(e, bx, by, box + 1, box + 1, box / 2);
            lx = bx + box + (aero ? 7 : 10);
        } else {
            if (aero) {
                eui_fillrect(bx, by, box, box, hover ? 0xFF3C7FB1u : 0xFF8E8F8Fu);
                eui_vgrad2(bx + 1, by + 1, box - 2, box - 2, 0,
                           hover ? 0xFFDEF9FAu : 0xFFDCDCDCu, hover ? 0xFFE6FBFCu : 0xFFEBEBEBu,
                           hover ? 0xFFEDFCFDu : 0xFFF5F5F5u, hover ? 0xFFF3FDFDu : 0xFFFFFFFFu);
                if (e->value) eui_check(bx + 1, by + 1, box - 2, e->enabled ? (e->color ? acc : 0xFF31437Cu) : eui_th.disabled_text);
            } else if (e->value) {
                eui_fillroundrect(bx, by, box, box, 4, on);
                eui_check(bx, by, box, 0xFFFFFFFFu);
            } else {
                eui_fillroundrect(bx, by, box, box, 4, hover ? acc : eui_th.border);
                eui_fillroundrect(bx + 2, by + 2, box - 4, box - 4, 3, eui_th.surface);
            }
            if (focus) eui_focus_ring(e, bx, by, box, box, aero ? 0 : 4);
            lx = bx + box + (aero ? 7 : 10);
        }
        eui_clip(lx, y, x + w - lx, h);
        eui_textbox(lx, y, x + w - lx, h, e->text, text, sz, EASYUI_ALIGN_LEFT);
        break;
    }
    case EASYUI_TEXTINPUT: {
        int32_t r = eui_radius(e, aero ? 2 : 6), pad = eui_pad(e, 8);
        uint32_t surface = !e->enabled ? eui_th.disabled : bg ? bg : eui_th.surface;
        radius = r;
        if (aero) {
            eui_fillroundrect(x, y, w, h, r, focus ? 0xFF3D7BADu : hover ? 0xFF5794BFu : 0xFFABADB3u);
            eui_fillroundrect(x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 1 : 0, surface);
        } else {
            eui_fillroundrect(x, y, w, h, r, focus ? acc : eui_th.border);
            eui_fillroundrect(x + (focus ? 2 : 1), y + (focus ? 2 : 1), w - (focus ? 4 : 2), h - (focus ? 4 : 2),
                              r > 1 ? r - 1 : 0, surface);
        }
        char shown[EASYUI_TEXT_MAX];
        int32_t len = eui_shown(e, shown), avail = w - 2 * pad;
        if (e->cursor > len) e->cursor = len;
        /* e->scroll is the pixel offset that keeps the caret in view */
        int32_t caret = eui_linew(shown, e->cursor, sz);
        if (caret - e->scroll > avail) e->scroll = caret - avail;
        if (caret - e->scroll < 0) e->scroll = caret;
        if (eui_linew(shown, len, sz) - e->scroll < avail) e->scroll = eui_linew(shown, len, sz) - avail;
        if (e->scroll < 0) e->scroll = 0;
        int32_t ty = y + (h - th) / 2;
        eui_clip(x + pad - 2, y + 2, w - 2 * pad + 4, h - 4);
        if (!len && !focus) eui_drawtext(x + pad, ty, e->placeholder, eui_th.muted, sz);
        else eui_drawline_text(x + pad - e->scroll, ty, shown, len, text, sz);
        if (focus && eui_caret_on)
            eui_fillrect(x + pad + caret - e->scroll, ty - 2, th > 20 ? 2 : 1, th + 4, aero ? text : acc);
        break;
    }
    case EASYUI_PROGRESS: {
        int32_t span = e->max - e->min;
        int32_t fw = span ? (int32_t)((int64_t)(e->value - e->min) * w / span) : 0;
        int32_t r = eui_radius(e, aero ? 2 : h / 2 < 6 ? h / 2 : 6);
        radius = r;
        if (aero) {
            /* the green Windows 7 bar, or setcolor()'s colour */
            uint32_t c = !e->enabled ? eui_th.disabled_text : e->color ? acc : 0xFF06B025u;
            eui_fillroundrect(x, y, w, h, r, 0xFFBCBCBCu);
            if (bg) eui_fillroundrect(x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 1 : 0, bg);
            else eui_vgrad2(x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 1 : 0, 0xFFF3F3F3u, 0xFFEBEBEBu, 0xFFDBDBDBu, 0xFFE3E3E3u);
            if (fw > 2) {
                eui_clip(x + 1, y + 1, fw - 2, h - 2);
                eui_vgrad2(x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 1 : 0,
                           eui_mix(c, 0xFFFFFFFFu, 140u), eui_mix(c, 0xFFFFFFFFu, 70u), c, eui_mix(c, 0xFFFFFFFFu, 50u));
            }
        } else {
            eui_fillroundrect(x, y, w, h, r, bg ? bg : eui_th.track);
            if (fw > 0) {
                eui_clip(x, y, fw, h);
                eui_fillroundrect(x, y, w, h, r, e->enabled ? acc : eui_th.disabled_text);
            }
        }
        break;
    }
    case EASYUI_DROPDOWN: {
        int32_t r = eui_radius(e, aero ? 3 : 6), pad = eui_pad(e, 10);
        uint32_t arrow = text;
        radius = r;
        if (aero && e->enabled) {
            eui_aero_button(x, y, w, h, r, hover || eui_open == id, eui_open == id, 0, bg);
            arrow = text = eui_textcolor(e, bg && !eui_light(bg) ? 0xFFFFFFFFu : 0xFF000000u);
        } else {
            uint32_t fill = !e->enabled ? eui_th.disabled
                          : bg ? (hover || eui_open == id ? eui_mix(bg, 0xFFFFFFFFu, 30u) : bg)
                          : (hover || eui_open == id) ? eui_th.button_hover : eui_th.button;
            eui_fillroundrect(x, y, w, h, r, fill);
        }
        if (focus) eui_focus_ring(e, x, y, w, h, r);
        int32_t cx = x + w - 16, cy = y + h / 2;
        eui_drawline(cx - 4, cy - 2, cx, cy + 2, arrow); eui_drawline(cx, cy + 2, cx + 4, cy - 2, arrow);
        eui_drawline(cx - 4, cy - 1, cx, cy + 3, arrow); eui_drawline(cx, cy + 3, cx + 4, cy - 1, arrow);
        eui_clip(x + pad - 2, y, w - pad - 24, h);
        eui_drawtext(x + pad, y + (h - th) / 2,
                     e->value >= 0 && e->value < e->count ? e->items[e->value] : e->text, text, sz);
        break;
    }
    case EASYUI_LISTBOX:
    case EASYUI_PANEL: {
        int panel = e->type == EASYUI_PANEL;
        int32_t r = eui_radius(e, aero ? (panel ? 3 : 0) : 8);
        uint32_t fill = bg ? bg : panel && e->color ? (0xFF000000u | e->color)
                      : aero && panel ? eui_mix(eui_th.background, 0xFFFFFFFFu, 90u) : eui_th.surface;
        radius = r;
        eui_fillroundrect(x, y, w, h, r, aero ? (panel ? 0xFFD5DFE5u : focus ? 0xFF3D7BADu : 0xFF828790u)
                                              : focus ? acc : eui_th.border);
        eui_fillroundrect(x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 1 : 0, fill);
        if (panel) {
            if (e->text[0]) {
                eui_clip(x + 12, y, w - 24, h);
                eui_drawtext(x + eui_pad(e, 14), y + 12, e->text,
                             eui_textcolor(e, aero ? 0xFF1E3287u : eui_th.muted), sz);
            }
            break;
        }
        {
            int32_t rh = eui_row_h(e), rows = eui_list_rows(e), pad = eui_pad(e, 10);
            int scroll = e->count > rows;
            for (int32_t i = 0; i < rows && i + e->scroll < e->count; i++) {
                int32_t idx = i + e->scroll, ry = y + 2 + i * rh;
                uint32_t row_text = text;
                eui_clip(x + 2, y + 2, w - 4, h - 4);
                if (aero && (idx == e->value || idx == e->item_hover)) {
                    eui_aero_row(x + 3, ry, w - 6 - (scroll ? 8 : 0), rh, idx == e->value, focus);
                } else if (idx == e->value) {
                    eui_fillroundrect(x + 3, ry, w - 6 - (scroll ? 6 : 0), rh, 5, acc);
                    row_text = eui_th.accent_text;
                } else if (idx == e->item_hover) {
                    eui_fillroundrect(x + 3, ry, w - 6 - (scroll ? 6 : 0), rh, 5, eui_th.button_hover);
                }
                eui_clip(x + 4, ry, w - 14, rh);
                eui_drawtext(x + pad, ry + (rh - th) / 2, e->items[idx], row_text, sz);
            }
            if (scroll) {
                eui_clip_all();
                int32_t track = h - 8, bar = track * rows / e->count;
                if (bar < 16) bar = 16;
                int32_t by = y + 4 + (track - bar) * e->scroll / (e->count - rows);
                if (aero) {
                    eui_fillrect(x + w - 10, y + 2, 8, h - 4, 0xFFF0F0F0u);
                    eui_aero_button(x + w - 10, by, 8, bar, 2, 0, 0, 0, 0);
                } else {
                    eui_fillroundrect(x + w - 7, by, 4, bar, 2, eui_th.border);
                }
            }
        }
        break;
    }
    case EASYUI_IMAGE:
        radius = eui_radius(e, 0);
        if (bg) eui_fillroundrect(x, y, w, h, radius, bg);
        if (e->image && e->image_w > 0 && e->image_h > 0) {
            eui_clip(x, y, w, h);
            for (int32_t py = 0; py < h; py++) {
                const uint32_t* src = e->image + (size_t)(py * e->image_h / h) * (size_t)e->image_w;
                for (int32_t px = 0; px < w; px++) eui_span(x + px, x + px + 1, y + py, src[px * e->image_w / w]);
            }
        }
        break;
    case EASYUI_CANVAS:
        radius = eui_radius(e, 0);
        if (bg) eui_fillroundrect(x, y, w, h, radius, bg);
        eui_clip(x, y, w, h);
        eui_fire(id, EASYUI_ONDRAW, 0);
        break;
    case EASYUI_SEPARATOR:
        eui_fillrect(x, y, w, 1, border ? bcol : aero ? 0xFFD5DFE5u : eui_th.border);
        if (aero && !border) eui_fillrect(x, y + 1, w, 1, 0xFFFFFFFFu);
        border = 0;
        break;
    default:
        break;
    }
    eui_clip_all();
    if (border) eui_strokeround(x, y, w, h, radius, bw, bcol);
    eui_use(0);
}

static void eui_paint_dropdown(void) {
    int32_t x, y, w, h;
    if (!eui_dd_rect(&x, &y, &w, &h)) return;
    eui_widget_t* e = &eui_w[eui_open];
    int32_t rh = eui_row_h(e), pad = eui_pad(e, 10);
    eui_use(e);
    if (eui_aero) {
        eui_fillrect(x + 2, y + 2, w, h, 0x30000000u);
        eui_fillrect(x, y, w, h, 0xFF646464u);
        eui_fillrect(x + 1, y + 1, w - 2, h - 2, e->background ? e->background : 0xFFFFFFFFu);
    } else {
        eui_fillroundrect(x - 1, y + 1, w + 2, h + 3, 8, 0x30000000u);
        eui_fillroundrect(x, y, w, h, 7, eui_th.border);
        eui_fillroundrect(x + 1, y + 1, w - 2, h - 2, 6, e->background ? e->background : eui_th.surface);
    }
    uint32_t text = eui_textcolor(e, eui_th.text);
    for (int32_t i = 0; i < 8 && i + e->scroll < e->count; i++) {
        int32_t idx = i + e->scroll, ry = y + 2 + i * rh;
        int hot = idx == e->item_hover;
        if (hot && eui_aero) eui_fillrect(x + 2, ry, w - 4, rh, 0xFF3399FFu);
        else if (hot) eui_fillroundrect(x + 3, ry, w - 6, rh, 5, eui_accent_of(e));
        eui_clip(x + 4, ry, w - 8, rh);
        eui_drawtext(x + pad, ry + (rh - eui_texth(e->size)) / 2, e->items[idx], hot ? eui_th.accent_text : text, e->size);
        if (idx == e->value) eui_fillrect(x + 4, ry + 6, 2, rh - 12, hot ? eui_th.accent_text : eui_accent_of(e));
        eui_clip_all();
    }
    eui_use(0);
}

static void eui_paint_message(void) {
    int32_t mw = eui_W - 40 < 380 ? eui_W - 40 : 380, mh = 170;
    int32_t x = (eui_W - mw) / 2, y = (eui_H - mh) / 2;
    int32_t bw = 90, bh = 30, bx = x + mw - bw - 16, by = y + mh - bh - 16;
    eui_use(0);
    eui_tpx = 0;      /* the window's font, at the message's own sizes */
    eui_fillrect(0, 0, eui_W, eui_H, 0x660A1218u);
    if (eui_aero) {
        /* white message area over a grey button strip, as in Windows 7 */
        eui_fillrect(x - 2, y + 2, mw + 6, mh + 4, 0x30000000u);
        eui_fillrect(x, y, mw, mh, 0xFF8E8F8Fu);
        eui_fillrect(x + 1, y + 1, mw - 2, mh - 2, 0xFFFFFFFFu);
        eui_fillrect(x + 1, by - 14, mw - 2, y + mh - 1 - (by - 14), 0xFFF0F0F0u);
        eui_fillrect(x + 1, by - 14, mw - 2, 1, 0xFFDFDFDFu);
        eui_clip(x + 20, y, mw - 40, mh);
        eui_drawtext(x + 20, y + 18, eui_msg_title, 0xFF1E3287u, 2);
        eui_textbox(x + 20, y + 48, mw - 40, mh - 110, eui_msg_text, 0xFF000000u, 1, EASYUI_ALIGN_LEFT);
        eui_clip_all();
        eui_aero_button(bx, by, bw, bh, 3, eui_msg_hover, 0, 1, 0);
        eui_drawtext(bx + (bw - eui_textwidth("OK", 1)) / 2, by + (bh - eui_texth(1)) / 2, "OK", 0xFF000000u, 1);
        return;
    }
    eui_fillroundrect(x - 3, y - 1, mw + 6, mh + 8, 14, 0x30000000u);
    eui_fillroundrect(x, y, mw, mh, 12, eui_th.border);
    eui_fillroundrect(x + 1, y + 1, mw - 2, mh - 2, 11, eui_th.background);
    eui_clip(x + 20, y, mw - 40, mh);
    eui_drawtext(x + 20, y + 18, eui_msg_title, eui_th.text, 2);
    eui_textbox(x + 20, y + 48, mw - 40, mh - 110, eui_msg_text, eui_th.muted, 1, EASYUI_ALIGN_LEFT);
    eui_clip_all();
    eui_fillroundrect(bx, by, bw, bh, 7, eui_msg_hover ? eui_th.accent_hover : eui_th.accent);
    eui_drawtext(bx + (bw - eui_textwidth("OK", 1)) / 2, by + (bh - eui_texth(1)) / 2, "OK", eui_th.accent_text, 1);
}

/* ---------------------------------------------------------------------
 *  File and folder pickers
 *
 *  A dialog drawn over the window, like messagebox: places on the left
 *  (Home and its folders, System), Home / System / Up buttons and the
 *  current folder on top, the folder's contents in the middle, and a name
 *  field with Cancel and Open / Save / Select at the bottom.
 * --------------------------------------------------------------------- */
#define EUI_PICK_ENTRIES 256
enum { EUI_PICK_OPEN = 1, EUI_PICK_FOLDER, EUI_PICK_SAVE };
/* hot spots: what the pointer is over */
enum { EUI_PH_NONE = 0, EUI_PH_CANCEL, EUI_PH_OK, EUI_PH_HOME, EUI_PH_SYSTEM, EUI_PH_UP,
       EUI_PH_NAME, EUI_PH_PLACE };

typedef struct {
    int mode;
    char title[64];
    char dir[EASYUI_PATH_MAX];
    char result[EASYUI_PATH_MAX];
    char filter[64];          /* "txt;md" */
    char name[64];
    char note[64];
    syscall_dirent_t* entries;
    int32_t count, scroll, selected, hover, hot, hot_place;
    long last_click;
    int32_t last_item;
    easyui_callback done;
} eui_pick_t;

static eui_pick_t eui_pk;

static void eui_pk_join(char* out, const char* dir, const char* name) {
    int n = 0;
    for (; dir[n] && n < EASYUI_PATH_MAX - 1; n++) out[n] = dir[n];
    if (n && out[n - 1] != '/' && n < EASYUI_PATH_MAX - 1) out[n++] = '/';
    for (int i = 0; name[i] && n < EASYUI_PATH_MAX - 1; i++) out[n++] = name[i];
    out[n] = '\0';
}

/* the user's home folder, from the username MOSdesk saved */
static void eui_pk_home(char* out) {
    eui_copy(out, "0:/users", EASYUI_PATH_MAX);
    if (eui_username[0]) {
        char path[EASYUI_PATH_MAX];
        eui_pk_join(path, "0:/users", eui_username);
        if (mos_is_dir(path) == 1) eui_copy(out, path, EASYUI_PATH_MAX);
    }
    if (mos_is_dir(out) != 1) eui_copy(out, "0:/", EASYUI_PATH_MAX);
}

static int eui_pk_lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* whether `name` ends in one of the ";"-separated extensions of the filter */
static int eui_pk_matches(const char* name) {
    const char* f = eui_pk.filter;
    int len = (int)strlen(name);
    if (!*f) return 1;
    while (*f) {
        int n = 0;
        while (f[n] && f[n] != ';') n++;
        const char* ext = f[0] == '.' ? f + 1 : f;
        int en = f[0] == '.' ? n - 1 : n;
        if (en > 0 && len > en && name[len - en - 1] == '.') {
            int same = 1;
            for (int i = 0; i < en && same; i++)
                same = eui_pk_lower(name[len - en + i]) == eui_pk_lower(ext[i]);
            if (same) return 1;
        }
        f += n;
        if (*f == ';') f++;
    }
    return 0;
}

static int eui_pk_compare(const syscall_dirent_t* a, const syscall_dirent_t* b) {
    int ad = a->type == SYSCALL_DIRENT_TYPE_DIR, bd = b->type == SYSCALL_DIRENT_TYPE_DIR;
    if (ad != bd) return ad ? -1 : 1;
    for (int i = 0;; i++) {
        int ca = eui_pk_lower((unsigned char)a->name[i]), cb = eui_pk_lower((unsigned char)b->name[i]);
        if (ca != cb) return ca < cb ? -1 : 1;
        if (!ca) return 0;
    }
}

/* Shows folder `dir`: folders first, then the files the picker offers. */
static void eui_pk_load(const char* dir) {
    if (mos_is_dir(dir) != 1) { eui_copy(eui_pk.note, "That folder is not available", 64); return; }
    if (dir != eui_pk.dir) eui_copy(eui_pk.dir, dir, EASYUI_PATH_MAX);
    if (!eui_pk.entries) eui_pk.entries = (syscall_dirent_t*)malloc(sizeof(syscall_dirent_t) * EUI_PICK_ENTRIES);
    eui_pk.count = 0; eui_pk.scroll = 0; eui_pk.selected = -1; eui_pk.hover = -1; eui_pk.note[0] = 0;
    eui_dirty = 1;
    if (!eui_pk.entries) return;
    long n = mos_syscall(SYS_LISTDIR, (long)(uintptr_t)eui_pk.dir, (long)(uintptr_t)eui_pk.entries, EUI_PICK_ENTRIES);
    if (n < 0) { eui_copy(eui_pk.note, "Unable to list this folder", 64); return; }
    if (n > EUI_PICK_ENTRIES) n = EUI_PICK_ENTRIES;
    for (long i = 0; i < n; i++) {
        syscall_dirent_t d = eui_pk.entries[i];
        int dir_entry = d.type == SYSCALL_DIRENT_TYPE_DIR;
        if (d.hidden || (!dir_entry && (eui_pk.mode == EUI_PICK_FOLDER || !eui_pk_matches(d.name)))) continue;
        int32_t at = eui_pk.count++;
        while (at > 0 && eui_pk_compare(&eui_pk.entries[at - 1], &d) > 0) {
            eui_pk.entries[at] = eui_pk.entries[at - 1];
            at--;
        }
        eui_pk.entries[at] = d;
    }
}

static void eui_pk_up(void) {
    char parent[EASYUI_PATH_MAX];
    int cut = -1;
    eui_copy(parent, eui_pk.dir, EASYUI_PATH_MAX);
    for (int i = 0; parent[i]; i++) if (parent[i] == '/' && parent[i + 1]) cut = i;
    if (cut < 0) return;
    parent[cut < 3 ? 3 : cut] = '\0';      /* keep the slash of "0:/" */
    eui_pk_load(parent);
}

static void eui_pk_finish(int chosen) {
    easyui_callback done = eui_pk.done;
    eui_pick_active = 0;
    free(eui_pk.entries);
    eui_pk.entries = 0;
    if (!chosen) eui_pk.result[0] = '\0';
    eui_dirty = 1;
    if (done) {
        int prev = eui_cur;
        eui_cur = 0;
        done(chosen);
        eui_cur = prev;
    }
}

/* Open / Save / Select: what pressing the main button (or Enter) does. */
static void eui_pk_accept(void) {
    syscall_dirent_t* sel = eui_pk.selected >= 0 && eui_pk.selected < eui_pk.count ? &eui_pk.entries[eui_pk.selected] : 0;
    int sel_dir = sel && sel->type == SYSCALL_DIRENT_TYPE_DIR;
    char path[EASYUI_PATH_MAX];
    if (eui_pk.mode == EUI_PICK_FOLDER) {
        if (sel_dir) eui_pk_join(eui_pk.result, eui_pk.dir, sel->name);
        else eui_copy(eui_pk.result, eui_pk.dir, EASYUI_PATH_MAX);
        eui_pk_finish(1);
    } else if (eui_pk.mode == EUI_PICK_SAVE) {
        if (sel_dir && (!eui_pk.name[0] || !strcmp(eui_pk.name, sel->name))) {
            eui_pk_join(path, eui_pk.dir, sel->name);
            eui_pk_load(path);
            return;
        }
        if (!eui_pk.name[0]) { eui_copy(eui_pk.note, "Type a name first", 64); eui_dirty = 1; return; }
        eui_pk_join(eui_pk.result, eui_pk.dir, eui_pk.name);
        if (mos_is_dir(eui_pk.result) == 1) { eui_pk_load(eui_pk.result); eui_pk.name[0] = 0; return; }
        eui_pk_finish(1);
    } else if (sel_dir) {
        eui_pk_join(path, eui_pk.dir, sel->name);
        eui_pk_load(path);
    } else if (sel) {
        eui_pk_join(eui_pk.result, eui_pk.dir, sel->name);
        eui_pk_finish(1);
    }
}

/* --- layout --- */
typedef struct { int32_t x, y, w, h; } eui_rect_t;
typedef struct {
    eui_rect_t box, home, system, up, path, places, list, name, cancel, ok;
    int32_t row_h, rows;
} eui_pick_layout_t;

static int eui_in(eui_rect_t r, int32_t x, int32_t y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

static eui_pick_layout_t eui_pk_layout(void) {
    eui_pick_layout_t L;
    int32_t w = eui_W - 16 < 600 ? eui_W - 16 : 600, h = eui_H - 16 < 440 ? eui_H - 16 : 440;
    int32_t x = (eui_W - w) / 2, y = (eui_H - h) / 2, places = w >= 440 ? 118 : 0;
    L.box = (eui_rect_t){x, y, w, h};
    L.home = (eui_rect_t){x + 12, y + 44, 58, 28};
    L.system = (eui_rect_t){x + 74, y + 44, 64, 28};
    L.up = (eui_rect_t){x + 142, y + 44, 40, 28};
    L.path = (eui_rect_t){x + 190, y + 44, w - 202, 28};
    L.cancel = (eui_rect_t){x + w - 12 - 96 - 8 - 96, y + h - 42, 96, 30};
    L.ok = (eui_rect_t){x + w - 12 - 96, y + h - 42, 96, 30};
    L.name = (eui_rect_t){x + 92, y + h - 82, w - 104, 28};
    L.places = (eui_rect_t){x + 12, y + 82, places - 8, h - 82 - 92};
    L.list = (eui_rect_t){x + 12 + places, y + 82, w - 24 - places, h - 82 - 92};
    L.row_h = 26;
    L.rows = (L.list.h - 4) / L.row_h;
    if (L.rows < 1) L.rows = 1;
    return L;
}

/* Places: Home, its standard folders, System. Returns how many. */
static int eui_pk_places(char paths[5][EASYUI_PATH_MAX], const char* names[5]) {
    static const char* const sub[3] = {"Documents", "Pictures", "Desktop"};
    int n = 0;
    eui_pk_home(paths[n]); names[n++] = "Home";
    for (int i = 0; i < 3; i++) {
        eui_pk_join(paths[n], paths[0], sub[i]);
        if (mos_is_dir(paths[n]) == 1) names[n++] = sub[i];
    }
    eui_copy(paths[n], "0:/", EASYUI_PATH_MAX); names[n++] = "System";
    return n;
}

static void eui_pk_scroll_to(int32_t index) {
    eui_pick_layout_t L = eui_pk_layout();
    if (index < eui_pk.scroll) eui_pk.scroll = index;
    if (index >= eui_pk.scroll + L.rows) eui_pk.scroll = index - L.rows + 1;
    if (eui_pk.scroll > eui_pk.count - L.rows) eui_pk.scroll = eui_pk.count - L.rows;
    if (eui_pk.scroll < 0) eui_pk.scroll = 0;
}

/* --- input --- */
static void eui_pick_mouse(int32_t x, int32_t y, int pressed) {
    eui_pick_layout_t L = eui_pk_layout();
    int hot = EUI_PH_NONE, place = -1;
    int32_t row = -1;
    char paths[5][EASYUI_PATH_MAX];
    const char* names[5];
    if (eui_in(L.cancel, x, y)) hot = EUI_PH_CANCEL;
    else if (eui_in(L.ok, x, y)) hot = EUI_PH_OK;
    else if (eui_in(L.home, x, y)) hot = EUI_PH_HOME;
    else if (eui_in(L.system, x, y)) hot = EUI_PH_SYSTEM;
    else if (eui_in(L.up, x, y)) hot = EUI_PH_UP;
    else if (L.places.w > 0 && eui_in(L.places, x, y) && (y - L.places.y - 4) / L.row_h < 5) {
        hot = EUI_PH_PLACE; place = (y - L.places.y - 4) / L.row_h;
    } else if (eui_in(L.list, x, y)) {
        row = (y - L.list.y - 2) / L.row_h + eui_pk.scroll;
        if (row >= eui_pk.count) row = -1;
    }
    if (hot != eui_pk.hot || place != eui_pk.hot_place || row != eui_pk.hover) {
        eui_pk.hot = hot; eui_pk.hot_place = place; eui_pk.hover = row; eui_dirty = 1;
    }
    if (!pressed) return;
    eui_pk.note[0] = '\0';
    if (hot == EUI_PH_CANCEL) { eui_pk_finish(0); return; }
    if (hot == EUI_PH_OK) { eui_pk_accept(); return; }
    if (hot == EUI_PH_HOME) { char home[EASYUI_PATH_MAX]; eui_pk_home(home); eui_pk_load(home); return; }
    if (hot == EUI_PH_SYSTEM) { eui_pk_load("0:/"); return; }
    if (hot == EUI_PH_UP) { eui_pk_up(); return; }
    if (hot == EUI_PH_PLACE) {
        int n = eui_pk_places(paths, names);
        if (place < n) eui_pk_load(paths[place]);
        return;
    }
    if (row >= 0) {
        long now = mos_uptime();
        int dbl = row == eui_pk.last_item && now - eui_pk.last_click < 450;
        eui_pk.last_item = row; eui_pk.last_click = now;
        eui_pk.selected = row;
        if (eui_pk.mode == EUI_PICK_SAVE && eui_pk.entries[row].type != SYSCALL_DIRENT_TYPE_DIR)
            eui_copy(eui_pk.name, eui_pk.entries[row].name, 64);
        eui_dirty = 1;
        if (dbl) {
            eui_pk.last_item = -1;
            if (eui_pk.entries[row].type == SYSCALL_DIRENT_TYPE_DIR) {
                char path[EASYUI_PATH_MAX];
                eui_pk_join(path, eui_pk.dir, eui_pk.entries[row].name);
                eui_pk_load(path);
            } else {
                eui_pk_accept();
            }
        }
    }
}

static void eui_pick_wheel(int32_t delta) {
    eui_pick_layout_t L = eui_pk_layout();
    eui_pk.scroll -= delta > 0 ? 3 : -3;
    if (eui_pk.scroll > eui_pk.count - L.rows) eui_pk.scroll = eui_pk.count - L.rows;
    if (eui_pk.scroll < 0) eui_pk.scroll = 0;
    eui_dirty = 1;
}

static void eui_pick_key(int key, int mods) {
    eui_dirty = 1;
    eui_caret_on = 1; eui_caret_time = mos_uptime();
    if (key == EASYUI_KEY_ESCAPE) { eui_pk_finish(0); return; }
    if (key == EASYUI_KEY_ENTER) { eui_pk_accept(); return; }
    if (key == EASYUI_KEY_UP || key == EASYUI_KEY_DOWN) {
        int32_t v = eui_pk.selected + (key == EASYUI_KEY_DOWN ? 1 : -1);
        if (eui_pk.selected < 0) v = 0;
        if (v >= 0 && v < eui_pk.count) {
            eui_pk.selected = v;
            eui_pk_scroll_to(v);
            if (eui_pk.mode == EUI_PICK_SAVE && eui_pk.entries[v].type != SYSCALL_DIRENT_TYPE_DIR)
                eui_copy(eui_pk.name, eui_pk.entries[v].name, 64);
        }
        return;
    }
    if (eui_pk.mode != EUI_PICK_SAVE) {
        if (key == EASYUI_KEY_BACKSPACE) eui_pk_up();
        return;
    }
    int32_t len = (int32_t)strlen(eui_pk.name);
    char c = (mods & SYSCALL_INPUT_MOD_CTRL) ? 0 : eui_ascii(key, (mods & SYSCALL_INPUT_MOD_SHIFT) != 0);
    if (key == EASYUI_KEY_BACKSPACE && len) eui_pk.name[len - 1] = '\0';
    else if (c && c != '/' && c != ':' && c != '\\' && len < 63) { eui_pk.name[len] = c; eui_pk.name[len + 1] = '\0'; }
    eui_pk.note[0] = '\0';
}

/* --- painting --- */
static void eui_pk_folder_icon(int x, int y) {
    eui_fillroundrect(x, y + 2, 7, 4, 1, 0xFFD39B2Au);
    eui_fillroundrect(x, y + 4, 16, 11, 2, 0xFFD9A537u);
    eui_fillroundrect(x + 1, y + 6, 14, 9, 1, 0xFFF4CF6Bu);
}

static void eui_pk_computer_icon(int x, int y) {
    eui_fillroundrect(x, y + 1, 16, 11, 1, 0xFF3D4B57u);
    eui_fillrect(x + 1, y + 2, 14, 9, 0xFF4A90C8u);
    eui_fillrect(x + 2, y + 3, 6, 2, 0xFFDDEEFFu);
    eui_fillrect(x + 6, y + 12, 4, 2, 0xFF6C7A86u);
    eui_fillrect(x + 3, y + 14, 10, 2, 0xFF8C99A3u);
}

static void eui_pk_file_icon(int x, int y, int app) {
    eui_fillrect(x + 2, y, 10, 16, 0xFF8C99A3u);
    eui_fillrect(x + 3, y + 1, 8, 14, 0xFFFFFFFFu);
    eui_fillrect(x + 10, y, 3, 3, 0xFF8C99A3u);
    eui_fillrect(x + 12, y + 3, 1, 13, 0xFF8C99A3u);
    eui_fillrect(x + 2, y + 15, 11, 1, 0xFF8C99A3u);
    if (app) eui_fillroundrect(x + 4, y + 5, 7, 7, 2, eui_th.accent);
    else for (int i = 0; i < 4; i++) eui_fillrect(x + 4, y + 4 + i * 3, 6, 1, 0xFFB6C0C8u);
}

/* a toolbar or dialog button in the current look */
static void eui_pk_button(eui_rect_t r, const char* label, int hot, int primary) {
    uint32_t fg;
    if (eui_aero) {
        eui_aero_button(r.x, r.y, r.w, r.h, 3, hot, 0, primary, 0);
        fg = 0xFF000000u;
    } else if (primary) {
        eui_fillroundrect(r.x, r.y, r.w, r.h, 7, hot ? eui_th.accent_hover : eui_th.accent);
        fg = eui_th.accent_text;
    } else {
        eui_fillroundrect(r.x, r.y, r.w, r.h, 7, hot ? eui_th.button_hover : eui_th.button);
        fg = eui_th.text;
    }
    eui_clip(r.x, r.y, r.w, r.h);
    eui_textbox(r.x + 4, r.y, r.w - 8, r.h, label, fg, 1, EASYUI_ALIGN_CENTER);
    eui_clip_all();
}

static void eui_paint_picker(void) {
    eui_pick_layout_t L = eui_pk_layout();
    eui_rect_t b = L.box;
    int aero = eui_aero;
    uint32_t panel = aero && !eui_dark ? 0xFFF0F0F0u : eui_th.background;
    uint32_t list_bg = eui_th.surface, text = eui_th.text;
    char paths[5][EASYUI_PATH_MAX];
    const char* names[5];
    eui_use(0);
    eui_tpx = 0;
    eui_fillrect(0, 0, eui_W, eui_H, 0x660A1218u);
    if (aero) {
        /* a glass frame in the accent colour around the dialog */
        eui_fillroundrect(b.x - 6, b.y - 4, b.w + 12, b.h + 12, 9, 0x30000000u);
        eui_vgrad2(b.x - 5, b.y - 5, b.w + 10, b.h + 10, 7,
                   eui_mix(eui_th.accent, 0xFFFFFFFFu, 150u), eui_mix(eui_th.accent, 0xFFFFFFFFu, 100u),
                   eui_mix(eui_th.accent, 0xFFFFFFFFu, 60u), eui_mix(eui_th.accent, 0xFFFFFFFFu, 90u));
        eui_strokeround(b.x - 5, b.y - 5, b.w + 10, b.h + 10, 7, 1, 0xC0102030u);
        eui_fillrect(b.x, b.y + 34, b.w, b.h - 34, panel);
        eui_fillrect(b.x - 1, b.y + 33, b.w + 2, 1, 0xA0243447u);
        eui_clip(b.x + 12, b.y, b.w - 24, 34);
        eui_drawtext(b.x + 12, b.y + 9, eui_pk.title, 0xFF0B1A2Bu, 2);
    } else {
        eui_fillroundrect(b.x - 3, b.y - 1, b.w + 6, b.h + 8, 14, 0x30000000u);
        eui_fillroundrect(b.x, b.y, b.w, b.h, 12, eui_th.border);
        eui_fillroundrect(b.x + 1, b.y + 1, b.w - 2, b.h - 2, 11, panel);
        eui_clip(b.x + 12, b.y, b.w - 24, 40);
        eui_drawtext(b.x + 14, b.y + 12, eui_pk.title, text, 2);
    }
    eui_clip_all();

    eui_pk_button(L.home, "Home", eui_pk.hot == EUI_PH_HOME, 0);
    eui_pk_button(L.system, "System", eui_pk.hot == EUI_PH_SYSTEM, 0);
    eui_pk_button(L.up, "Up", eui_pk.hot == EUI_PH_UP, 0);
    eui_fillroundrect(L.path.x, L.path.y, L.path.w, L.path.h, aero ? 2 : 6, aero ? 0xFFABADB3u : eui_th.border);
    eui_fillroundrect(L.path.x + 1, L.path.y + 1, L.path.w - 2, L.path.h - 2, aero ? 1 : 5, list_bg);
    eui_pk_folder_icon(L.path.x + 8, L.path.y + 6);
    {
        /* the end of a long path is the useful part */
        const char* shown = eui_pk.dir;
        while (*shown && eui_textwidth(shown, 1) > L.path.w - 40) shown++;
        eui_clip(L.path.x + 30, L.path.y, L.path.w - 36, L.path.h);
        eui_drawtext(L.path.x + 30, L.path.y + (L.path.h - eui_texth(1)) / 2, shown, text, 1);
        eui_clip_all();
    }

    if (L.places.w > 0) {
        int n = eui_pk_places(paths, names);
        eui_fillrect(L.places.x, L.places.y, L.places.w, L.places.h, aero ? 0xFFF1F5FBu : eui_th.surface);
        eui_drawrect(L.places.x, L.places.y, L.places.w, L.places.h, aero ? 0xFFD5DFE5u : eui_th.border);
        for (int i = 0; i < n; i++) {
            int32_t ry = L.places.y + 4 + i * L.row_h;
            int here = !strcmp(paths[i], eui_pk.dir);
            if (aero && (here || eui_pk.hot_place == i))
                eui_aero_row(L.places.x + 3, ry, L.places.w - 6, L.row_h - 2, here, 1);
            else if (here || eui_pk.hot_place == i)
                eui_fillroundrect(L.places.x + 3, ry, L.places.w - 6, L.row_h - 2, 5,
                                  here ? (eui_th.accent & 0x00FFFFFFu) | 0x40000000u : eui_th.button_hover);
            if (i == n - 1) eui_pk_computer_icon(L.places.x + 8, ry + 4);
            else eui_pk_folder_icon(L.places.x + 8, ry + 5);
            eui_clip(L.places.x, ry, L.places.w - 4, L.row_h);
            eui_drawtext(L.places.x + 30, ry + (L.row_h - 2 - eui_texth(1)) / 2, names[i], text, 1);
            eui_clip_all();
        }
    }

    eui_fillrect(L.list.x, L.list.y, L.list.w, L.list.h, aero ? 0xFF828790u : eui_th.border);
    eui_fillrect(L.list.x + 1, L.list.y + 1, L.list.w - 2, L.list.h - 2, list_bg);
    int scroll = eui_pk.count > L.rows;
    for (int32_t i = 0; i < L.rows && i + eui_pk.scroll < eui_pk.count; i++) {
        int32_t idx = i + eui_pk.scroll, ry = L.list.y + 2 + i * L.row_h;
        const syscall_dirent_t* d = &eui_pk.entries[idx];
        int dir = d->type == SYSCALL_DIRENT_TYPE_DIR;
        int32_t rw = L.list.w - 4 - (scroll ? 10 : 0);
        uint32_t fg = text;
        if (aero && (idx == eui_pk.selected || idx == eui_pk.hover))
            eui_aero_row(L.list.x + 2, ry, rw, L.row_h - 1, idx == eui_pk.selected, 1);
        else if (idx == eui_pk.selected) { eui_fillroundrect(L.list.x + 2, ry, rw, L.row_h - 1, 5, eui_th.accent); fg = eui_th.accent_text; }
        else if (idx == eui_pk.hover) eui_fillroundrect(L.list.x + 2, ry, rw, L.row_h - 1, 5, eui_th.button_hover);
        if (dir) eui_pk_folder_icon(L.list.x + 8, ry + 5);
        else {
            int n = (int)strlen(d->name);
            eui_pk_file_icon(L.list.x + 8, ry + 4, n > 4 && !strcmp(d->name + n - 4, ".run"));
        }
        eui_clip(L.list.x + 2, ry, rw - 4, L.row_h);
        eui_drawtext(L.list.x + 32, ry + (L.row_h - 1 - eui_texth(1)) / 2, d->name, fg, 1);
        eui_clip_all();
    }
    if (!eui_pk.count)
        eui_drawtext(L.list.x + 12, L.list.y + 12,
                     eui_pk.mode == EUI_PICK_FOLDER ? "No folders here" : "This folder is empty",
                     eui_th.muted, 1);
    if (scroll) {
        int32_t track = L.list.h - 8, bar = track * L.rows / eui_pk.count;
        if (bar < 16) bar = 16;
        int32_t by = L.list.y + 4 + (track - bar) * eui_pk.scroll / (eui_pk.count - L.rows);
        if (aero) eui_aero_button(L.list.x + L.list.w - 11, by, 8, bar, 2, 0, 0, 0, 0);
        else eui_fillroundrect(L.list.x + L.list.w - 8, by, 4, bar, 2, eui_th.border);
    }

    /* Name row: the name to save under, or what is selected. */
    {
        const char* label = eui_pk.mode == EUI_PICK_SAVE ? "File name:"
                          : eui_pk.mode == EUI_PICK_FOLDER ? "Folder:" : "File:";
        char shown[EASYUI_PATH_MAX];
        const syscall_dirent_t* sel = eui_pk.selected >= 0 && eui_pk.selected < eui_pk.count
            ? &eui_pk.entries[eui_pk.selected] : 0;
        eui_drawtext(b.x + 12, L.name.y + (L.name.h - eui_texth(1)) / 2, label, text, 1);
        if (eui_pk.mode == EUI_PICK_SAVE) eui_copy(shown, eui_pk.name, sizeof shown);
        else if (eui_pk.mode == EUI_PICK_FOLDER)
            eui_copy(shown, sel && sel->type == SYSCALL_DIRENT_TYPE_DIR ? sel->name : eui_pk.dir, sizeof shown);
        else eui_copy(shown, sel && sel->type != SYSCALL_DIRENT_TYPE_DIR ? sel->name : "", sizeof shown);
        eui_fillroundrect(L.name.x, L.name.y, L.name.w, L.name.h, aero ? 2 : 6,
                          eui_pk.mode == EUI_PICK_SAVE ? (aero ? 0xFF3D7BADu : eui_th.accent) : (aero ? 0xFFABADB3u : eui_th.border));
        eui_fillroundrect(L.name.x + 1, L.name.y + 1, L.name.w - 2, L.name.h - 2, aero ? 1 : 5,
                          eui_pk.mode == EUI_PICK_SAVE ? list_bg : panel);
        eui_clip(L.name.x + 4, L.name.y, L.name.w - 8, L.name.h);
        int32_t ty = L.name.y + (L.name.h - eui_texth(1)) / 2;
        eui_drawtext(L.name.x + 8, ty, shown, text, 1);
        if (eui_pk.mode == EUI_PICK_SAVE && eui_caret_on)
            eui_fillrect(L.name.x + 8 + eui_textwidth(shown, 1), ty - 2, 1, eui_texth(1) + 4, text);
        eui_clip_all();
    }
    if (eui_pk.note[0]) {
        eui_clip(b.x + 12, L.ok.y, L.cancel.x - b.x - 20, L.ok.h);
        eui_drawtext(b.x + 12, L.ok.y + (L.ok.h - eui_texth(1)) / 2, eui_pk.note, 0xFFB43A32u, 1);
        eui_clip_all();
    }
    eui_pk_button(L.cancel, "Cancel", eui_pk.hot == EUI_PH_CANCEL, 0);
    eui_pk_button(L.ok, eui_pk.mode == EUI_PICK_SAVE ? "Save" : eui_pk.mode == EUI_PICK_FOLDER ? "Select" : "Open",
                  eui_pk.hot == EUI_PH_OK, 1);
}

/* --- API --- */
static int eui_pick_start(int mode, const char* title, const char* folder, const char* filter,
                          const char* name, easyui_callback done) {
    if (!eui_win || eui_pick_active) return 0;
    memset(&eui_pk, 0, sizeof eui_pk);
    eui_pk.mode = mode;
    eui_pk.done = done;
    eui_pk.last_item = -1;
    eui_pk.hot_place = -1;
    eui_copy(eui_pk.title, title && *title ? title
             : mode == EUI_PICK_SAVE ? "Save as" : mode == EUI_PICK_FOLDER ? "Choose a folder" : "Open a file", 64);
    eui_copy(eui_pk.filter, filter, 64);
    eui_copy(eui_pk.name, name, 64);
    eui_pick_active = 1;
    /* the window's widgets let go of the pointer while the picker is up */
    if (eui_valid(eui_hover)) { eui_w[eui_hover].hovered = 0; eui_fire(eui_hover, EASYUI_ONLEAVE, 0); }
    eui_hover = 0; eui_pressed = 0; eui_open = 0;
    if (folder && *folder && mos_is_dir(folder) == 1) eui_pk_load(folder);
    else { char home[EASYUI_PATH_MAX]; eui_pk_home(home); eui_pk_load(home); }
    return 1;
}

static int eui_pickfile(const char* title, const char* folder, const char* extensions, easyui_callback done) {
    return eui_pick_start(EUI_PICK_OPEN, title, folder, extensions, 0, done);
}
static int eui_pickfolder(const char* title, const char* folder, easyui_callback done) {
    return eui_pick_start(EUI_PICK_FOLDER, title, folder, 0, 0, done);
}
static int eui_savefile(const char* title, const char* folder, const char* name, easyui_callback done) {
    return eui_pick_start(EUI_PICK_SAVE, title, folder, 0, name, done);
}
static const char* eui_pickedpath(void) { return eui_pk.result; }

static void eui_paint(void) {
    if (!eui_px) return;
    eui_clip_all();
    eui_fillrect(0, 0, eui_W, eui_H, eui_w[0].background ? eui_w[0].background : eui_th.background);
    for (int id = 1; id <= EASYUI_MAX_WIDGETS; id++)
        if (eui_w[id].used && eui_w[id].visible) eui_paint_widget(id);
    if (eui_open) eui_paint_dropdown();
    if (eui_pick_active) eui_paint_picker();
    if (eui_msg_active) eui_paint_message();
    eui_clip_all();
    eui_use(0);
}

/* ---------------------------------------------------------------------
 *  Window and main loop
 * --------------------------------------------------------------------- */
static int eui_init(const char* title, int width, int height) {
    if (eui_win) return 1;
    if (width < 64) width = 64;
    if (height < 48) height = 48;
    eui_poll_theme();
    eui_make_theme();
    eui_load_default_fonts();
    eui_last_poll = mos_uptime();
    eui_win = mos_window_create(0, 120, 80, (uint32_t)width, (uint32_t)height, title ? title : "App");
    if (!eui_win) return 0;
    if (!eui_resize(width, height)) { mos_window_close(eui_win); eui_win = 0; return 0; }
    eui_running = 1;
    eui_dirty = 1;
    eui_w[0].used = 1;
    return 1;
}

static void eui_quit(void) { eui_running = 0; }

static int eui_update(void) {
    syscall_window_event_t ev;
    if (!eui_running || !eui_win) return 0;
    while (eui_running && mos_window_next_event(eui_win, &ev) == SYS_SUCCESS) {
        switch (ev.type) {
        case SYSCALL_WINDOW_EVENT_MOUSE: eui_mouse(ev.x, ev.y, ev.buttons); break;
        case SYSCALL_WINDOW_EVENT_MOUSE_WHEEL: eui_wheel(ev.wheel); break;
        case SYSCALL_WINDOW_EVENT_KEY: if (ev.buttons) eui_key((int)ev.key, ev.mods); break;
        case SYSCALL_WINDOW_EVENT_RESIZED:
            if (eui_resize(ev.width, ev.height)) eui_fire(EASYUI_WINDOW, EASYUI_ONRESIZE, ev.width);
            break;
        case SYSCALL_WINDOW_EVENT_CLOSE_REQUEST:
            if (eui_w[0].cb[EASYUI_ONCLOSE]) eui_fire(EASYUI_WINDOW, EASYUI_ONCLOSE, 0);
            else eui_running = 0;
            break;
        case SYSCALL_WINDOW_EVENT_FOCUS_LOST:
            if (eui_valid(eui_hover)) { eui_w[eui_hover].hovered = 0; eui_fire(eui_hover, EASYUI_ONLEAVE, 0); }
            eui_hover = 0; eui_pressed = 0; eui_buttons = 0; eui_open = 0; eui_dirty = 1;
            break;
        default: break;
        }
    }
    long now = mos_uptime();
    if (now - eui_last_poll >= 500) { eui_last_poll = now; eui_poll_theme(); }
    for (int i = 0; i < EASYUI_MAX_TIMERS; i++) {
        if (!eui_timers[i].fn || now < eui_timers[i].next) continue;
        eui_timers[i].next = (int32_t)now + eui_timers[i].interval;
        int prev = eui_cur;
        eui_cur = 0;
        eui_timers[i].fn(i + 1);
        eui_cur = prev;
        eui_dirty = 1;
    }
    eui_fire(EASYUI_WINDOW, EASYUI_ONFRAME, (int)now);
    if (((eui_valid(eui_focus) && eui_w[eui_focus].type == EASYUI_TEXTINPUT) ||
         (eui_pick_active && eui_pk.mode == EUI_PICK_SAVE)) && now - eui_caret_time >= 530) {
        eui_caret_on = !eui_caret_on; eui_caret_time = now; eui_dirty = 1;
    }
    if (eui_dirty && eui_running) {
        eui_dirty = 0;
        eui_paint();
        mos_window_present(eui_win, eui_px, (uint32_t)eui_W * 4u, 0, 0, (uint32_t)eui_W, (uint32_t)eui_H);
    }
    return eui_running;
}

static int eui_run(void) {
    while (eui_update()) mos_sleep(16);
    if (eui_win) { mos_window_close(eui_win); eui_win = 0; }
    return 0;
}

static void eui_settitle(const char* t) { if (eui_win) mos_window_set_title(eui_win, t); }
static int eui_width(void) { return eui_W; }
static int eui_height(void) { return eui_H; }
static uint32_t eui_window(void) { return eui_win; }
static void eui_redraw(void) { eui_dirty = 1; }
static void eui_setdarkmode(int on) { eui_dark = on != 0; eui_make_theme(); eui_dirty = 1; }
static void eui_setaccent(uint32_t c) { eui_accent_override = c; eui_make_theme(); eui_dirty = 1; }
static uint32_t eui_accent(void) { if (!eui_th.accent) eui_make_theme(); return eui_th.accent; }
static const easyui_theme_t* eui_theme(void) { if (!eui_th.accent) eui_make_theme(); return &eui_th; }

/* --- creating widgets --- */
static easyui_widget eui_createbutton(int x, int y, int w, int h, const char* t) { return eui_new(EASYUI_BUTTON, x, y, w, h, t); }
static easyui_widget eui_createlabel(int x, int y, int w, int h, const char* t) { return eui_new(EASYUI_LABEL, x, y, w, h, t); }
static easyui_widget eui_createslider(int x, int y, int w, int h, int min, int max, int value) {
    easyui_widget id = eui_new(EASYUI_SLIDER, x, y, w, h, 0);
    if (id) { eui_w[id].min = min; eui_w[id].max = max; eui_w[id].value = eui_clampv(&eui_w[id], value); }
    return id;
}
static easyui_widget eui_createcheckbox(int x, int y, int w, int h, const char* t, int checked) {
    easyui_widget id = eui_new(EASYUI_CHECKBOX, x, y, w, h, t);
    if (id) eui_w[id].value = checked != 0;
    return id;
}
static easyui_widget eui_createradio(int x, int y, int w, int h, const char* t, int group) {
    easyui_widget id = eui_new(EASYUI_RADIO, x, y, w, h, t);
    if (!id) return 0;
    eui_w[id].group = group;
    /* the first radio button of a group starts selected */
    int first = 1;
    for (int i = 1; i <= EASYUI_MAX_WIDGETS; i++)
        if (i != id && eui_w[i].used && eui_w[i].type == EASYUI_RADIO && eui_w[i].group == group) first = 0;
    eui_w[id].value = first;
    return id;
}
static easyui_widget eui_createtoggle(int x, int y, int w, int h, const char* t, int on) {
    easyui_widget id = eui_new(EASYUI_TOGGLE, x, y, w, h, t);
    if (id) eui_w[id].value = on != 0;
    return id;
}
static easyui_widget eui_createtextinput(int x, int y, int w, int h, const char* placeholder) {
    easyui_widget id = eui_new(EASYUI_TEXTINPUT, x, y, w, h, 0);
    if (id) eui_copy(eui_w[id].placeholder, placeholder, (int)sizeof eui_w[id].placeholder);
    return id;
}
static easyui_widget eui_createprogress(int x, int y, int w, int h, int max) {
    easyui_widget id = eui_new(EASYUI_PROGRESS, x, y, w, h, 0);
    if (id) eui_w[id].max = max;
    return id;
}
static easyui_widget eui_createdropdown(int x, int y, int w, int h) {
    easyui_widget id = eui_new(EASYUI_DROPDOWN, x, y, w, h, "Choose...");
    if (id) eui_w[id].value = -1;
    return id;
}
static easyui_widget eui_createlistbox(int x, int y, int w, int h) {
    easyui_widget id = eui_new(EASYUI_LISTBOX, x, y, w, h, 0);
    if (id) eui_w[id].value = -1;
    return id;
}
static easyui_widget eui_createpanel(int x, int y, int w, int h, const char* t) { return eui_new(EASYUI_PANEL, x, y, w, h, t); }
static easyui_widget eui_createimage(int x, int y, int w, int h, const uint32_t* p, int iw, int ih) {
    easyui_widget id = eui_new(EASYUI_IMAGE, x, y, w, h, 0);
    if (id) { eui_w[id].image = p; eui_w[id].image_w = iw; eui_w[id].image_h = ih; }
    return id;
}
static easyui_widget eui_createcanvas(int x, int y, int w, int h) { return eui_new(EASYUI_CANVAS, x, y, w, h, 0); }
static easyui_widget eui_createseparator(int x, int y, int w) { return eui_new(EASYUI_SEPARATOR, x, y, w, 1, 0); }

static void eui_clearitems(easyui_widget id) {
    if (!eui_valid(id)) return;
    eui_widget_t* e = &eui_w[id];
    for (int i = 0; i < e->count; i++) free(e->items[i]);
    free(e->items);
    e->items = 0; e->count = 0; e->capacity = 0; e->scroll = 0;
    if (e->type == EASYUI_LISTBOX || e->type == EASYUI_DROPDOWN) e->value = -1;
    eui_dirty = 1;
}

static void eui_remove(easyui_widget id) {
    if (!eui_valid(id)) return;
    eui_clearitems(id);
    if (eui_focus == id) eui_focus = 0;
    if (eui_hover == id) eui_hover = 0;
    if (eui_pressed == id) eui_pressed = 0;
    if (eui_open == id) eui_open = 0;
    memset(&eui_w[id], 0, sizeof eui_w[id]);
    eui_dirty = 1;
}

/* --- events --- */
static int eui_recallfunc(easyui_widget id, int event, easyui_callback fn) {
    if (event < 0 || event >= EASYUI_EVENT_COUNT || id < 0 || id > EASYUI_MAX_WIDGETS) return 0;
    if (id && !eui_w[id].used) return 0;
    eui_w[id].cb[event] = fn;
    return 1;
}
static easyui_widget eui_current(void) { return eui_cur; }

/* --- properties --- */
static void eui_settext(easyui_widget id, const char* t) {
    if (!eui_valid(id)) return;
    eui_copy(eui_w[id].text, t, EASYUI_TEXT_MAX);
    eui_w[id].cursor = (int32_t)strlen(eui_w[id].text);
    eui_dirty = 1;
}
static const char* eui_gettext(easyui_widget id) {
    if (!eui_valid(id)) return "";
    eui_widget_t* e = &eui_w[id];
    if ((e->type == EASYUI_DROPDOWN || e->type == EASYUI_LISTBOX) && e->value >= 0 && e->value < e->count) return e->items[e->value];
    return e->text;
}
static void eui_setvalue(easyui_widget id, int v) {
    if (!eui_valid(id)) return;
    eui_widget_t* e = &eui_w[id];
    if (e->type == EASYUI_RADIO && v) { eui_select_radio(id); return; }
    if (e->type == EASYUI_LISTBOX || e->type == EASYUI_DROPDOWN) {
        if (v < -1 || v >= e->count) return;
        if (e->type == EASYUI_LISTBOX && v >= 0) eui_list_scroll_to(e, v);
    }
    if (e->type == EASYUI_CHECKBOX || e->type == EASYUI_TOGGLE || e->type == EASYUI_RADIO) v = v != 0;
    eui_change(id, v);
}
static int eui_getvalue(easyui_widget id) { return eui_valid(id) ? eui_w[id].value : 0; }
static void eui_setrange(easyui_widget id, int min, int max) {
    if (!eui_valid(id)) return;
    eui_w[id].min = min; eui_w[id].max = max;
    eui_w[id].value = eui_clampv(&eui_w[id], eui_w[id].value);
    eui_dirty = 1;
}
static void eui_setstep(easyui_widget id, int step) { if (eui_valid(id)) eui_w[id].step = step < 1 ? 1 : step; }
static int eui_additem(easyui_widget id, const char* t) {
    if (!eui_valid(id)) return -1;
    eui_widget_t* e = &eui_w[id];
    if (e->count == e->capacity) {
        int32_t cap = e->capacity ? e->capacity * 2 : 8;
        char** grown = (char**)realloc(e->items, (size_t)cap * sizeof(char*));
        if (!grown) return -1;
        e->items = grown; e->capacity = cap;
    }
    int32_t n = (int32_t)strlen(t ? t : "");
    char* copy = (char*)malloc((size_t)n + 1u);
    if (!copy) return -1;
    eui_copy(copy, t, n + 1);
    e->items[e->count] = copy;
    eui_dirty = 1;
    return e->count++;
}
static const char* eui_getitem(easyui_widget id, int i) {
    if (!eui_valid(id) || i < 0 || i >= eui_w[id].count) return "";
    return eui_w[id].items[i];
}
static int eui_itemcount(easyui_widget id) { return eui_valid(id) ? eui_w[id].count : 0; }
static void eui_setenabled(easyui_widget id, int on) {
    if (!eui_valid(id)) return;
    eui_w[id].enabled = on != 0;
    if (!on && eui_focus == id) eui_set_focus(0);
    eui_dirty = 1;
}
static void eui_setvisible(easyui_widget id, int on) {
    if (!eui_valid(id)) return;
    eui_w[id].visible = on != 0;
    if (!on && eui_focus == id) eui_set_focus(0);
    if (!on && eui_open == id) eui_open = 0;
    eui_dirty = 1;
}
static void eui_setposition(easyui_widget id, int x, int y) {
    if (!eui_valid(id)) return;
    eui_widget_t* e = &eui_w[id];
    e->x = x; e->y = y;
    e->margin_right = eui_W - (x + e->w); e->margin_bottom = eui_H - (y + e->h);
    eui_dirty = 1;
}
static void eui_setsize(easyui_widget id, int w, int h) {
    if (!eui_valid(id)) return;
    eui_widget_t* e = &eui_w[id];
    e->w = w < 1 ? 1 : w; e->h = h < 1 ? 1 : h;
    e->margin_right = eui_W - (e->x + e->w); e->margin_bottom = eui_H - (e->y + e->h);
    eui_dirty = 1;
}
static void eui_setanchor(easyui_widget id, uint32_t a) {
    if (!eui_valid(id)) return;
    eui_widget_t* e = &eui_w[id];
    e->anchor = a;
    e->margin_right = eui_W - (e->x + e->w); e->margin_bottom = eui_H - (e->y + e->h);
}
static void eui_setcolor(easyui_widget id, uint32_t c) { if (eui_valid(id)) { eui_w[id].color = c; eui_dirty = 1; } }
static void eui_settextsize(easyui_widget id, int s) {
    if (!eui_valid(id)) return;
    eui_w[id].size = s < 1 ? 1 : s > 8 ? 8 : s;
    eui_w[id].size_set = 1;
    eui_w[id].font_px = 0;
    eui_dirty = 1;
}
static void eui_setalign(easyui_widget id, int a) { if (eui_valid(id)) { eui_w[id].align = (uint8_t)a; eui_dirty = 1; } }
static void eui_setstyle(easyui_widget id, int s) { if (eui_valid(id)) { eui_w[id].style = (uint8_t)s; eui_dirty = 1; } }
static void eui_setpassword(easyui_widget id, int on) { if (eui_valid(id)) { eui_w[id].password = on != 0; eui_dirty = 1; } }
static void eui_setmaxlength(easyui_widget id, int n) {
    if (!eui_valid(id)) return;
    eui_w[id].maxlength = n < 0 ? 0 : n > EASYUI_TEXT_MAX - 1 ? EASYUI_TEXT_MAX - 1 : n;
}
static void eui_setimage(easyui_widget id, const uint32_t* p, int iw, int ih) {
    if (!eui_valid(id)) return;
    eui_w[id].image = p; eui_w[id].image_w = iw; eui_w[id].image_h = ih;
    eui_dirty = 1;
}
static void eui_setfocus_api(easyui_widget id) { eui_set_focus(id); }
static easyui_widget eui_focused(void) { return eui_focus; }
static int eui_type(easyui_widget id) { return eui_valid(id) ? eui_w[id].type : EASYUI_NONE; }
static int eui_getx(easyui_widget id) { return eui_valid(id) ? eui_w[id].x : 0; }
static int eui_gety(easyui_widget id) { return eui_valid(id) ? eui_w[id].y : 0; }
static int eui_getwidth(easyui_widget id) { return eui_valid(id) ? eui_w[id].w : 0; }
static int eui_getheight(easyui_widget id) { return eui_valid(id) ? eui_w[id].h : 0; }

/* --- timers and dialogs --- */
static int eui_settimer(int ms, easyui_callback fn) {
    if (!fn) return 0;
    for (int i = 0; i < EASYUI_MAX_TIMERS; i++)
        if (!eui_timers[i].fn) {
            eui_timers[i].fn = fn;
            eui_timers[i].interval = ms < 1 ? 1 : ms;
            eui_timers[i].next = (int32_t)mos_uptime() + eui_timers[i].interval;
            return i + 1;
        }
    return 0;
}
static void eui_stoptimer(int t) { if (t >= 1 && t <= EASYUI_MAX_TIMERS) eui_timers[t - 1].fn = 0; }
static void eui_messagebox(const char* title, const char* text) {
    eui_copy(eui_msg_title, title, (int)sizeof eui_msg_title);
    eui_copy(eui_msg_text, text, (int)sizeof eui_msg_text);
    eui_msg_active = 1; eui_msg_hover = 0; eui_open = 0; eui_pressed = 0;
    eui_dirty = 1;
}

static uint32_t* eui_pixels(void) { return eui_px; }

/* --- styling: a widget, or EASYUI_WINDOW (0) for every widget's default --- */
static int eui_stylable(easyui_widget id) { return id == EASYUI_WINDOW || eui_valid(id); }
static void eui_setfont(easyui_widget id, int font) {
    if (!eui_stylable(id) || font < 0 || font >= EASYUI_FONT_CUSTOM + eui_cfont_count) return;
    eui_w[id].font = font; eui_dirty = 1;
}
static void eui_setfontsize(easyui_widget id, int px) {
    if (!eui_stylable(id)) return;
    eui_w[id].font_px = px <= 0 ? 0 : px < 6 ? 6 : px > 128 ? 128 : px;
    if (!px) eui_w[id].size_set = 0;
    eui_dirty = 1;
}
static void eui_settextcolor(easyui_widget id, uint32_t c) {
    if (!eui_stylable(id)) return;
    eui_w[id].text_color = c && !(c >> 24) ? (c | 0xFF000000u) : c; eui_dirty = 1;
}
static void eui_setbackground(easyui_widget id, uint32_t c) {
    if (!eui_stylable(id)) return;
    eui_w[id].background = c && !(c >> 24) ? (c | 0xFF000000u) : c; eui_dirty = 1;
}
static void eui_setborder(easyui_widget id, uint32_t c, int width) {
    if (!eui_stylable(id)) return;
    eui_w[id].has_border = width >= 0;
    eui_w[id].border_width = width < 0 ? 0 : width > 16 ? 16 : width;
    eui_w[id].border_color = c && !(c >> 24) ? (c | 0xFF000000u) : c;
    eui_dirty = 1;
}
static void eui_setradius(easyui_widget id, int r) {
    if (!eui_stylable(id)) return;
    eui_w[id].has_radius = r >= 0; eui_w[id].radius = r < 0 ? 0 : r; eui_dirty = 1;
}
static void eui_setpadding(easyui_widget id, int px) {
    if (!eui_stylable(id)) return;
    eui_w[id].has_padding = px >= 0; eui_w[id].padding = px < 0 ? 0 : px; eui_dirty = 1;
}
static void eui_copystyle(easyui_widget to, easyui_widget from) {
    if (!eui_stylable(to) || !eui_stylable(from)) return;
    eui_widget_t* a = &eui_w[to];
    const eui_widget_t* b = &eui_w[from];
    a->font = b->font; a->font_px = b->font_px; a->size = b->size ? b->size : 1; a->size_set = b->size_set;
    a->text_color = b->text_color; a->background = b->background;
    a->border_color = b->border_color; a->border_width = b->border_width; a->has_border = b->has_border;
    a->radius = b->radius; a->has_radius = b->has_radius;
    a->padding = b->padding; a->has_padding = b->has_padding;
    a->align = b->align; a->style = b->style; a->color = b->color;
    eui_dirty = 1;
}

/* drawtext/textwidth for canvases: the font of the canvas (or the window's
 * default), at text size `size` */
static void eui_drawtext_api(int x, int y, const char* t, uint32_t c, int size) {
    int px = eui_tpx;
    eui_tpx = 0;
    eui_drawtext(x, y, t, c, size);
    eui_tpx = px;
}
static int eui_textwidth_api(const char* t, int size) {
    int px = eui_tpx;
    eui_tpx = 0;
    int w = eui_textwidth(t, size);
    eui_tpx = px;
    return w;
}
static void eui_drawtextfont(int x, int y, const char* t, uint32_t c, int font, int px) {
    int f0 = eui_tfont, p0 = eui_tpx;
    eui_tfont = font; eui_tpx = px > 0 ? px : 0;
    eui_drawtext(x, y, t, c, 1);
    eui_tfont = f0; eui_tpx = p0;
}
static int eui_textwidthfont(const char* t, int font, int px) {
    int f0 = eui_tfont, p0 = eui_tpx;
    eui_tfont = font; eui_tpx = px > 0 ? px : 0;
    int w = eui_textwidth(t, 1);
    eui_tfont = f0; eui_tpx = p0;
    return w;
}

static void eui_mouse_api(int* x, int* y, int* buttons) {
    if (x) *x = eui_mx;
    if (y) *y = eui_my;
    if (buttons) *buttons = (int)eui_buttons;
}

static void eui_setlook(int look) {
    eui_look = look == EASYUI_LOOK_MODERN || look == EASYUI_LOOK_AERO ? look : EASYUI_LOOK_AUTO;
    eui_make_theme();
    eui_dirty = 1;
}
static int eui_getlook(void) { if (!eui_th.accent) eui_make_theme(); return eui_aero ? EASYUI_LOOK_AERO : EASYUI_LOOK_MODERN; }

const easyui_api_t easyui = {
    eui_init, eui_run, eui_update, eui_quit, eui_settitle, eui_width, eui_height, eui_window, eui_redraw,
    eui_setdarkmode, eui_setaccent, eui_accent, eui_theme,
    eui_loadfont, eui_setsmoothtext, eui_smoothtext,
    eui_createbutton, eui_createlabel, eui_createslider, eui_createcheckbox, eui_createradio,
    eui_createtoggle, eui_createtextinput, eui_createprogress, eui_createdropdown, eui_createlistbox,
    eui_createpanel, eui_createimage, eui_createcanvas, eui_createseparator, eui_remove,
    eui_recallfunc, eui_current,
    eui_settext, eui_gettext, eui_setvalue, eui_getvalue, eui_setrange, eui_setstep,
    eui_additem, eui_clearitems, eui_getitem, eui_itemcount,
    eui_setenabled, eui_setvisible, eui_setposition, eui_setsize, eui_setanchor, eui_setcolor,
    eui_settextsize, eui_setalign, eui_setstyle, eui_setpassword, eui_setmaxlength, eui_setimage,
    eui_setfocus_api, eui_focused, eui_type, eui_getx, eui_gety, eui_getwidth, eui_getheight,
    eui_settimer, eui_stoptimer, eui_messagebox,
    eui_pixels, eui_fillrect, eui_drawrect, eui_fillroundrect, eui_fillcircle, eui_drawline,
    eui_drawtext_api, eui_textwidth_api, eui_drawpixels,
    eui_addfont, eui_setfont, eui_setfontsize, eui_settextcolor, eui_setbackground, eui_setborder,
    eui_setradius, eui_setpadding, eui_copystyle, eui_drawtextfont, eui_textwidthfont,
    eui_setlook, eui_getlook,
    eui_pickfile, eui_pickfolder, eui_savefile, eui_pickedpath,
    eui_mouse_api
};

#endif /* EASYUI_NO_IMPLEMENTATION */
#endif /* MINIMALOS_EASYUI_H */
