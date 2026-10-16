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
 * boxes, panels, images and canvases. Colours follow the MOSdesk theme
 * chosen in Settings > Appearance (the accent colour in
 * 0:/programs/mosdesk/settings.dat), and change while the app runs when the
 * user picks another theme.
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
    if (eui_dark) {
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

/* Reads the accent colour MOSdesk Settings saved. Every layout of
 * settings.dat starts with magic, mouse_speed, accent_color. */
static void eui_poll_theme(void) {
    uint32_t head[3] = {0, 0, 0};
    long fd = mos_open("0:/programs/mosdesk/settings.dat", SYS_O_RDONLY);
    if (fd <= 0) return;
    long n = mos_read(fd, head, sizeof head);
    mos_close(fd);
    if (n != (long)sizeof head || head[0] != 0x4D445331u || head[2] == eui_settings_accent) return;
    eui_settings_accent = head[2];
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

static void eui_fillroundrect(int x, int y, int w, int h, int r, uint32_t c) {
    if (w <= 0 || h <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    for (int32_t dy = 0; dy < h; dy++) {
        int32_t in = eui_inset(dy, h, r);
        eui_span(x + in, x + w - in, y + dy, c);
    }
}

/* a rounded rectangle `t` pixels thick */
static void eui_strokeround(int x, int y, int w, int h, int r, int t, uint32_t c) {
    if (w <= 0 || h <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    int ri = r > t ? r - t : 0;
    for (int32_t dy = 0; dy < h; dy++) {
        int32_t in = eui_inset(dy, h, r);
        if (dy < t || dy >= h - t) { eui_span(x + in, x + w - in, y + dy, c); continue; }
        int32_t inner = eui_inset(dy - t, h - 2 * t, ri) + t;
        eui_span(x + in, x + inner, y + dy, c);
        eui_span(x + w - inner, x + w - in, y + dy, c);
    }
}

static void eui_fillcircle(int cx, int cy, int r, uint32_t c) {
    eui_fillroundrect(cx - r, cy - r, r * 2 + 1, r * 2 + 1, r + 1, c);
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

static int eui_textwidth(const char* s, int size) {
    int n = 0, best = 0;
    if (size < 1) size = 1;
    for (; s && *s; s++) { if (*s == '\n') { if (n > best) best = n; n = 0; } else n++; }
    if (n > best) best = n;
    return best * 8 * size;
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

static void eui_drawtext(int x, int y, const char* s, uint32_t c, int size) {
    int cx = x;
    if (size < 1) size = 1;
    for (; s && *s; s++) {
        if (*s == '\n') { cx = x; y += 10 * size; continue; }
        if (cx >= eui_cx1) continue;
        if (cx + 8 * size > eui_cx0) eui_char(cx, y, *s, c, size);
        cx += 8 * size;
    }
}

static void eui_drawpixels(int x, int y, const uint32_t* p, int w, int h) {
    if (!p) return;
    for (int py = 0; py < h; py++)
        for (int px = 0; px < w; px++) {
            uint32_t c = p[(size_t)py * (size_t)w + (size_t)px];
            eui_span(x + px, x + px + 1, y + py, c);
        }
}

/* Draws `text` word-wrapped inside a box, vertically centred. */
static void eui_textbox(int32_t x, int32_t y, int32_t w, int32_t h, const char* text,
                        uint32_t color, int size, int align) {
    char line[EASYUI_TEXT_MAX + 1];
    int pass, lines = 0, max = w / (8 * size);
    int32_t ty = 0;
    if (max < 1) max = 1;
    if (max > EASYUI_TEXT_MAX) max = EASYUI_TEXT_MAX;
    for (pass = 0; pass < 2; pass++) {
        const char* s = text;
        int n = 0;
        if (pass == 1) ty = y + (h - (lines * 10 * size - 2 * size)) / 2;
        while (1) {
            /* the longest run of whole words that fits */
            int len = 0, cut = -1;
            while (s[len] && s[len] != '\n' && len < max) { if (s[len] == ' ') cut = len; len++; }
            if (s[len] && s[len] != '\n' && s[len] != ' ' && cut > 0) len = cut;
            if (pass == 1) {
                for (int i = 0; i < len; i++) line[i] = s[i];
                line[len] = '\0';
                int32_t lw = len * 8 * size;
                int32_t lx = align == EASYUI_ALIGN_CENTER ? x + (w - lw) / 2
                           : align == EASYUI_ALIGN_RIGHT ? x + w - lw : x;
                eui_drawtext(lx, ty, line, color, size);
                ty += 10 * size;
            }
            n++;
            s += len;
            if (*s == ' ' || *s == '\n') s++;
            if (!*s) break;
        }
        lines = n;
    }
}

/* ---------------------------------------------------------------------
 *  Widgets
 * --------------------------------------------------------------------- */
static int eui_valid(easyui_widget id) { return id > 0 && id <= EASYUI_MAX_WIDGETS && eui_w[id].used; }

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

static int32_t eui_row_h(eui_widget_t* e) { return 8 * e->size + 12; }

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
        int32_t pos = (eui_mx - e->x - 8) / (8 * e->size) + e->scroll;
        int32_t len = (int32_t)strlen(e->text);
        e->cursor = pos < 0 ? 0 : pos > len ? len : pos;
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

static void eui_mouse(int32_t x, int32_t y, uint32_t buttons) {
    int down = (buttons & 1u) != 0, was = (eui_buttons & 1u) != 0;
    eui_mx = x; eui_my = y; eui_buttons = buttons;
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
    int id = eui_open ? eui_open : eui_hover;
    if (!eui_valid(id)) return;
    eui_widget_t* e = &eui_w[id];
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

static void eui_focus_ring(eui_widget_t* e, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r) {
    (void)e;
    eui_strokeround(x - 3, y - 3, w + 6, h + 6, r + 3, 2, (eui_th.accent & 0x00FFFFFFu) | 0xA0000000u);
}

static void eui_check(int32_t x, int32_t y, int32_t s, uint32_t c) {
    int t = s / 8 > 1 ? s / 8 : 1;
    eui_thickline(x + s * 2 / 9, y + s / 2, x + s * 4 / 10, y + s * 7 / 10, t, c);
    eui_thickline(x + s * 4 / 10, y + s * 7 / 10, x + s * 3 / 4, y + s * 3 / 10, t, c);
}

static void eui_paint_widget(int id) {
    eui_widget_t* e = &eui_w[id];
    int32_t x = e->x, y = e->y, w = e->w, h = e->h, sz = e->size;
    uint32_t text = e->enabled ? eui_th.text : eui_th.disabled_text;
    uint32_t acc = eui_accent_of(e);
    int focus = id == eui_focus, hover = e->hovered && e->enabled, pressed = id == eui_pressed && hover;
    int32_t th = 8 * sz;
    switch (e->type) {
    case EASYUI_BUTTON: {
        uint32_t bg, fg = eui_th.text;
        int32_t r = h / 2 < 7 ? h / 2 : 7;
        if (!e->enabled) { bg = eui_th.disabled; fg = eui_th.disabled_text; }
        else if (e->style == EASYUI_SECONDARY) bg = pressed ? eui_th.button_pressed : hover ? eui_th.button_hover : eui_th.button;
        else if (e->style == EASYUI_FLAT) { bg = pressed ? eui_th.button_pressed : hover ? eui_th.button_hover : 0; fg = acc; }
        else {
            uint32_t base = e->style == EASYUI_DANGER ? eui_th.danger : acc;
            bg = pressed ? eui_mix(base, 0xFF000000u, 46u) : hover ? eui_mix(base, 0xFFFFFFFFu, 36u) : base;
            fg = eui_th.accent_text;
        }
        if (bg) eui_fillroundrect(x, y, w, h, r, bg);
        if (focus) eui_focus_ring(e, x, y, w, h, r);
        eui_clip(x + 4, y, w - 8, h);
        eui_textbox(x + 8, y, w - 16, h, e->text, fg, sz, e->align);
        break;
    }
    case EASYUI_LABEL:
        eui_clip(x, y, w, h);
        eui_textbox(x, y, w, h, e->text, e->color ? (0xFF000000u | e->color) : text, sz, e->align);
        break;
    case EASYUI_SLIDER: {
        int vertical = eui_slider_vertical(e);
        int32_t len = vertical ? h : w, thick = vertical ? w : h;
        int32_t r = thick / 2 < 9 ? thick / 2 : 9;
        int32_t span = e->max - e->min;
        int32_t pos = r + (span ? (int32_t)((int64_t)(e->value - e->min) * (len - 2 * r) / span) : 0);
        uint32_t fill = e->enabled ? acc : eui_th.disabled_text;
        if (vertical) {
            int32_t cx = x + w / 2, ky = y + h - pos;
            eui_fillroundrect(cx - 2, y + r - 2, 4, h - 2 * r + 4, 2, eui_th.track);
            eui_fillroundrect(cx - 2, ky, 4, y + h - r + 2 - ky, 2, fill);
            if (hover || focus || id == eui_pressed) eui_fillcircle(cx, ky, r + 3, (fill & 0x00FFFFFFu) | 0x40000000u);
            eui_fillcircle(cx, ky, r, fill);
            eui_fillcircle(cx, ky, r - 3, eui_th.surface);
        } else {
            int32_t cy = y + h / 2, kx = x + pos;
            eui_fillroundrect(x + r - 2, cy - 2, w - 2 * r + 4, 4, 2, eui_th.track);
            eui_fillroundrect(x + r - 2, cy - 2, kx - x - r + 2, 4, 2, fill);
            if (hover || focus || id == eui_pressed) eui_fillcircle(kx, cy, r + 3, (fill & 0x00FFFFFFu) | 0x40000000u);
            eui_fillcircle(kx, cy, r, fill);
            eui_fillcircle(kx, cy, r - 3, eui_th.surface);
        }
        break;
    }
    case EASYUI_CHECKBOX:
    case EASYUI_RADIO:
    case EASYUI_TOGGLE: {
        int32_t box = 10 + th, bx = x, by = y + (h - box) / 2, lx;
        uint32_t on = e->enabled ? acc : eui_th.disabled_text;
        if (e->type == EASYUI_TOGGLE) {
            int32_t tw = box * 2 - 2;
            eui_fillroundrect(bx, by, tw, box, box / 2, e->value ? on : (hover ? eui_th.button_pressed : eui_th.track));
            eui_fillcircle(e->value ? bx + tw - box / 2 - 1 : bx + box / 2, by + box / 2, box / 2 - 3, 0xFFFFFFFFu);
            if (focus) eui_focus_ring(e, bx, by, tw, box, box / 2);
            lx = bx + tw + 10;
        } else if (e->type == EASYUI_RADIO) {
            int32_t cx = bx + box / 2, cy = by + box / 2;
            eui_fillcircle(cx, cy, box / 2, e->value ? on : (hover ? acc : eui_th.border));
            eui_fillcircle(cx, cy, box / 2 - 2, eui_th.surface);
            if (e->value) eui_fillcircle(cx, cy, box / 2 - 5, on);
            if (focus) eui_focus_ring(e, bx, by, box + 1, box + 1, box / 2);
            lx = bx + box + 10;
        } else {
            if (e->value) {
                eui_fillroundrect(bx, by, box, box, 4, on);
                eui_check(bx, by, box, 0xFFFFFFFFu);
            } else {
                eui_fillroundrect(bx, by, box, box, 4, hover ? acc : eui_th.border);
                eui_fillroundrect(bx + 2, by + 2, box - 4, box - 4, 3, eui_th.surface);
            }
            if (focus) eui_focus_ring(e, bx, by, box, box, 4);
            lx = bx + box + 10;
        }
        eui_clip(lx, y, x + w - lx, h);
        eui_textbox(lx, y, x + w - lx, h, e->text, text, sz, EASYUI_ALIGN_LEFT);
        break;
    }
    case EASYUI_TEXTINPUT: {
        eui_fillroundrect(x, y, w, h, 6, focus ? acc : eui_th.border);
        eui_fillroundrect(x + (focus ? 2 : 1), y + (focus ? 2 : 1), w - (focus ? 4 : 2), h - (focus ? 4 : 2), 5,
                          e->enabled ? eui_th.surface : eui_th.disabled);
        int32_t cw = 8 * sz, visible = (w - 16) / cw, len = (int32_t)strlen(e->text);
        if (visible < 1) visible = 1;
        if (e->cursor > len) e->cursor = len;
        if (e->cursor < e->scroll) e->scroll = e->cursor;
        if (e->cursor > e->scroll + visible - 1) e->scroll = e->cursor - visible + 1;
        if (e->scroll < 0) e->scroll = 0;
        int32_t ty = y + (h - th) / 2;
        eui_clip(x + 6, y + 2, w - 12, h - 4);
        if (!len && !focus) eui_drawtext(x + 8, ty, e->placeholder, eui_th.muted, sz);
        else {
            char shown[EASYUI_TEXT_MAX];
            int32_t n = 0;
            for (int32_t i = e->scroll; i < len && n < visible + 1; i++) shown[n++] = e->password ? '*' : e->text[i];
            shown[n] = '\0';
            eui_drawtext(x + 8, ty, shown, text, sz);
        }
        if (focus && eui_caret_on) eui_fillrect(x + 8 + (e->cursor - e->scroll) * cw, ty - 2, sz > 1 ? 2 : 1, th + 4, acc);
        break;
    }
    case EASYUI_PROGRESS: {
        int32_t span = e->max - e->min;
        int32_t fw = span ? (int32_t)((int64_t)(e->value - e->min) * w / span) : 0;
        int32_t r = h / 2 < 6 ? h / 2 : 6;
        eui_fillroundrect(x, y, w, h, r, eui_th.track);
        if (fw > 0) {
            eui_clip(x, y, fw, h);
            eui_fillroundrect(x, y, w, h, r, e->enabled ? acc : eui_th.disabled_text);
        }
        break;
    }
    case EASYUI_DROPDOWN: {
        uint32_t bg = !e->enabled ? eui_th.disabled : (hover || eui_open == id) ? eui_th.button_hover : eui_th.button;
        eui_fillroundrect(x, y, w, h, 6, bg);
        if (focus) eui_focus_ring(e, x, y, w, h, 6);
        int32_t cx = x + w - 16, cy = y + h / 2;
        eui_drawline(cx - 4, cy - 2, cx, cy + 2, text); eui_drawline(cx, cy + 2, cx + 4, cy - 2, text);
        eui_drawline(cx - 4, cy - 1, cx, cy + 3, text); eui_drawline(cx, cy + 3, cx + 4, cy - 1, text);
        eui_clip(x + 8, y, w - 34, h);
        eui_drawtext(x + 10, y + (h - th) / 2,
                     e->value >= 0 && e->value < e->count ? e->items[e->value] : e->text, text, sz);
        break;
    }
    case EASYUI_LISTBOX:
    case EASYUI_PANEL:
        eui_fillroundrect(x, y, w, h, 8, focus ? acc : eui_th.border);
        eui_fillroundrect(x + 1, y + 1, w - 2, h - 2, 7, e->type == EASYUI_PANEL && e->color ? (0xFF000000u | e->color) : eui_th.surface);
        if (e->type == EASYUI_PANEL) {
            if (e->text[0]) { eui_clip(x + 12, y, w - 24, h); eui_drawtext(x + 14, y + 12, e->text, eui_th.muted, sz); }
            break;
        }
        {
            int32_t rh = eui_row_h(e), rows = eui_list_rows(e);
            int scroll = e->count > rows;
            for (int32_t i = 0; i < rows && i + e->scroll < e->count; i++) {
                int32_t idx = i + e->scroll, ry = y + 2 + i * rh;
                eui_clip(x + 2, y + 2, w - 4, h - 4);
                if (idx == e->value) eui_fillroundrect(x + 3, ry, w - 6 - (scroll ? 6 : 0), rh, 5, acc);
                else if (idx == e->item_hover) eui_fillroundrect(x + 3, ry, w - 6 - (scroll ? 6 : 0), rh, 5, eui_th.button_hover);
                eui_clip(x + 4, ry, w - 14, rh);
                eui_drawtext(x + 10, ry + 6, e->items[idx], idx == e->value ? eui_th.accent_text : text, sz);
            }
            if (scroll) {
                eui_clip_all();
                int32_t track = h - 8, bar = track * rows / e->count;
                if (bar < 16) bar = 16;
                int32_t by = y + 4 + (track - bar) * e->scroll / (e->count - rows);
                eui_fillroundrect(x + w - 7, by, 4, bar, 2, eui_th.border);
            }
        }
        break;
    case EASYUI_IMAGE:
        if (e->image && e->image_w > 0 && e->image_h > 0) {
            eui_clip(x, y, w, h);
            for (int32_t py = 0; py < h; py++) {
                const uint32_t* src = e->image + (size_t)(py * e->image_h / h) * (size_t)e->image_w;
                for (int32_t px = 0; px < w; px++) eui_span(x + px, x + px + 1, y + py, src[px * e->image_w / w]);
            }
        }
        break;
    case EASYUI_CANVAS:
        eui_clip(x, y, w, h);
        eui_fire(id, EASYUI_ONDRAW, 0);
        break;
    case EASYUI_SEPARATOR:
        eui_fillrect(x, y, w, 1, eui_th.border);
        break;
    default:
        break;
    }
    eui_clip_all();
}

static void eui_paint_dropdown(void) {
    int32_t x, y, w, h;
    if (!eui_dd_rect(&x, &y, &w, &h)) return;
    eui_widget_t* e = &eui_w[eui_open];
    int32_t rh = eui_row_h(e);
    eui_fillroundrect(x - 1, y + 1, w + 2, h + 3, 8, 0x30000000u);
    eui_fillroundrect(x, y, w, h, 7, eui_th.border);
    eui_fillroundrect(x + 1, y + 1, w - 2, h - 2, 6, eui_th.surface);
    for (int32_t i = 0; i < 8 && i + e->scroll < e->count; i++) {
        int32_t idx = i + e->scroll, ry = y + 2 + i * rh;
        if (idx == e->item_hover) eui_fillroundrect(x + 3, ry, w - 6, rh, 5, eui_accent_of(e));
        eui_clip(x + 4, ry, w - 8, rh);
        eui_drawtext(x + 10, ry + 6, e->items[idx], idx == e->item_hover ? eui_th.accent_text : eui_th.text, e->size);
        if (idx == e->value) eui_fillrect(x + 4, ry + 6, 2, rh - 12, idx == e->item_hover ? eui_th.accent_text : eui_accent_of(e));
        eui_clip_all();
    }
}

static void eui_paint_message(void) {
    int32_t mw = eui_W - 40 < 380 ? eui_W - 40 : 380, mh = 170;
    int32_t x = (eui_W - mw) / 2, y = (eui_H - mh) / 2;
    eui_fillrect(0, 0, eui_W, eui_H, 0x660A1218u);
    eui_fillroundrect(x - 3, y - 1, mw + 6, mh + 8, 14, 0x30000000u);
    eui_fillroundrect(x, y, mw, mh, 12, eui_th.border);
    eui_fillroundrect(x + 1, y + 1, mw - 2, mh - 2, 11, eui_th.background);
    eui_clip(x + 20, y, mw - 40, mh);
    eui_drawtext(x + 20, y + 20, eui_msg_title, eui_th.text, 2);
    eui_textbox(x + 20, y + 48, mw - 40, mh - 110, eui_msg_text, eui_th.muted, 1, EASYUI_ALIGN_LEFT);
    eui_clip_all();
    int32_t bw = 90, bh = 30, bx = x + mw - bw - 16, by = y + mh - bh - 16;
    eui_fillroundrect(bx, by, bw, bh, 7, eui_msg_hover ? eui_th.accent_hover : eui_th.accent);
    eui_drawtext(bx + (bw - 16) / 2, by + (bh - 8) / 2, "OK", eui_th.accent_text, 1);
}

static void eui_paint(void) {
    if (!eui_px) return;
    eui_clip_all();
    eui_fillrect(0, 0, eui_W, eui_H, eui_th.background);
    for (int id = 1; id <= EASYUI_MAX_WIDGETS; id++)
        if (eui_w[id].used && eui_w[id].visible) eui_paint_widget(id);
    if (eui_open) eui_paint_dropdown();
    if (eui_msg_active) eui_paint_message();
    eui_clip_all();
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
    if (eui_valid(eui_focus) && eui_w[eui_focus].type == EASYUI_TEXTINPUT && now - eui_caret_time >= 530) {
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
static void eui_settextsize(easyui_widget id, int s) { if (eui_valid(id)) { eui_w[id].size = s < 1 ? 1 : s > 8 ? 8 : s; eui_dirty = 1; } }
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

const easyui_api_t easyui = {
    eui_init, eui_run, eui_update, eui_quit, eui_settitle, eui_width, eui_height, eui_window, eui_redraw,
    eui_setdarkmode, eui_setaccent, eui_accent, eui_theme,
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
    eui_drawtext, eui_textwidth, eui_drawpixels
};

#endif /* EASYUI_NO_IMPLEMENTATION */
#endif /* MINIMALOS_EASYUI_H */
