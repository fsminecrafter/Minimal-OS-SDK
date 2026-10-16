/*
 * easyui demo - every widget in one mosWM window.
 *
 *     ./build.sh examples/easyui-demo.c
 *     run 0:/programs/easyui-demo.run      (inside MOSdesk)
 */
#include "easyui.h"
#include "stdio.h"

static int button1, counter, slider1, volume, check1, toggle1, name, greeting;
static int progress1, colours, list1, size_small, size_large, canvas1;
static int clicks;

static void number(char* out, int v) {
    char t[12]; int n = 0, neg = v < 0;
    if (neg) v = -v;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    if (neg) *out++ = '-';
    while (n) *out++ = t[--n];
    *out = '\0';
}

static void on_click(int value) {
    char text[40] = "Clicked ";
    (void)value;
    number(text + 8, ++clicks);
    easyui.settext(counter, text);
}

static void on_first_click(int value) {
    (void)value;
    easyui.messagebox("Welcome", "That was your first click. ONCE callbacks only run one time.");
}

static void on_slider(int value) {
    char text[32] = "Volume ";
    number(text + 7, value);
    easyui.settext(volume, text);
    easyui.setvalue(progress1, value);
}

static void on_dark(int on) { easyui.setdarkmode(on); }

static void on_check(int checked) { easyui.setenabled(slider1, checked); }

static void on_name(int length) {
    char text[EASYUI_TEXT_MAX + 8] = "Hello, ";
    const char* who = easyui.gettext(name);
    int i = 0;
    for (; who[i] && i < EASYUI_TEXT_MAX; i++) text[7 + i] = who[i];
    text[7 + i] = '\0';
    easyui.settext(greeting, length ? text : "Type your name and press Enter");
}

static void on_submit(int length) {
    (void)length;
    easyui.additem(list1, easyui.gettext(name));
    easyui.settext(name, "");
    on_name(0);
}

static void on_colour(int index) {
    static const uint32_t colours_argb[] = { 0, 0xFFD66B83u, 0xFF69A86Au, 0xFFE1B64Cu };
    easyui.setaccent(colours_argb[index]);
}

static void on_size(int selected) {
    if (!selected) return;
    easyui.settextsize(list1, easyui.current() == size_large ? 2 : 1);
}

static void on_list_open(int index) {
    easyui.messagebox("List", easyui.getitem(list1, index));
}

static void draw_canvas(int value) {
    int x = easyui.getx(canvas1), y = easyui.gety(canvas1);
    int w = easyui.getwidth(canvas1), h = easyui.getheight(canvas1);
    (void)value;
    easyui.fillroundrect(x, y, w, h, 8, 0xFF17232Au);
    for (int i = 0; i < 6; i++)
        easyui.fillcircle(x + 20 + i * (w - 40) / 5, y + h / 2, 6 + i * 2, easyui.accent());
    easyui.drawtext(x + 10, y + 8, "canvas", 0xFFFFFFFFu, 1);
}

static void tick(int timer) {
    static int seconds;
    char title[40] = "easyui demo - ";
    (void)timer;
    number(title + 14, ++seconds);
    title[14 + strlen(title + 14)] = '\0';
    strcpy(title + strlen(title), " s");
    easyui.settitle(title);
}

int main(void) {
    if (!easyui.init("easyui demo", 560, 420)) { mos_puts("no window\n"); return 1; }

    easyui.createpanel(12, 12, 260, 190, "Buttons and sliders");
    button1 = easyui.createbutton(28, 40, 110, 34, "Click me!");
    easyui.recallfunc(button1, ONCLICK, on_click);
    easyui.recallfunc(button1, ONCE, on_first_click);
    counter = easyui.createlabel(150, 40, 110, 34, "Not clicked");
    slider1 = easyui.createslider(28, 86, 228, 24, 0, 100, 40);
    easyui.recallfunc(slider1, ONCHANGE, on_slider);
    volume = easyui.createlabel(28, 112, 228, 18, "Volume 40");
    progress1 = easyui.createprogress(28, 138, 228, 10, 100);
    easyui.setvalue(progress1, 40);
    check1 = easyui.createcheckbox(28, 160, 120, 24, "Slider on", 1);
    easyui.recallfunc(check1, ONCHANGE, on_check);
    toggle1 = easyui.createtoggle(150, 160, 110, 24, "Dark", 0);
    easyui.recallfunc(toggle1, ONCHANGE, on_dark);

    easyui.createpanel(284, 12, 264, 190, "Text");
    name = easyui.createtextinput(300, 40, 232, 32, "Your name");
    easyui.recallfunc(name, ONCHANGE, on_name);
    easyui.recallfunc(name, ONSUBMIT, on_submit);
    greeting = easyui.createlabel(300, 80, 232, 40, "Type your name and press Enter");
    size_small = easyui.createradio(300, 130, 112, 24, "Small list", 1);
    size_large = easyui.createradio(420, 130, 112, 24, "Large list", 1);
    easyui.recallfunc(size_small, ONCHANGE, on_size);
    easyui.recallfunc(size_large, ONCHANGE, on_size);
    colours = easyui.createdropdown(300, 160, 232, 30);
    easyui.additem(colours, "MOSdesk theme");
    easyui.additem(colours, "Rose");
    easyui.additem(colours, "Forest");
    easyui.additem(colours, "Gold");
    easyui.setvalue(colours, 0);
    easyui.recallfunc(colours, ONCHANGE, on_colour);

    list1 = easyui.createlistbox(12, 214, 260, 194);
    easyui.additem(list1, "Names you enter appear here");
    easyui.additem(list1, "Double-click an item");
    easyui.recallfunc(list1, ONSUBMIT, on_list_open);
    easyui.setanchor(list1, EASYUI_ANCHOR_LEFT | EASYUI_ANCHOR_TOP | EASYUI_ANCHOR_BOTTOM);

    canvas1 = easyui.createcanvas(284, 214, 264, 194);
    easyui.recallfunc(canvas1, ONDRAW, draw_canvas);
    easyui.setanchor(canvas1, EASYUI_ANCHOR_LEFT | EASYUI_ANCHOR_TOP | EASYUI_ANCHOR_RIGHT | EASYUI_ANCHOR_BOTTOM);

    easyui.settimer(1000, tick);
    return easyui.run();
}
