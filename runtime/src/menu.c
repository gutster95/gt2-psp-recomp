/* In-game settings menu: L + R + SELECT opens it anywhere (title, menus, race). The game thread stops inside rt_poll
 * while the menu is open (the game is paused, SPU output muted); the menu is drawn with pspdebug's 8x8 font into the
 * picture on screen. Every option except 16:9 takes effect at once; closing writes settings.txt. */
#include <stdio.h>
#include <string.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspdebug.h>
#include <pspkernel.h>
#include "rt.h"

extern int g_fps60, g_fx60, g_lowlod, g_performance, g_show_fps, g_widescreen;
extern volatile int g_spu_paused;
void io_fps60_changed(void);

#define COMBO (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER | PSP_CTRL_SELECT)

static int g_ws_next = -1;   /* 16:9 for the next start */

enum { M_MODE, M_FPS60, M_FX, M_LOD, M_FPS, M_WS, M_CLOSE, M_COUNT };

static int mode_of(void)     /* 0 quality, 1 performance, 2 custom */
{
    if (!g_fps60 && !g_lowlod) return 0;
    if (g_fps60 && !g_fx60 && g_lowlod) return 1;
    return 2;
}

static void save_settings(void)
{
    static const char *const KEYS[] = {"performance", "fps60", "fps60_effects", "lowlod", "fps", "widescreen"};
    char path[300], keep[2048] = "", line[256];
    snprintf(path, sizeof path, "%ssettings.txt", g_basedir);
    FILE *f = fopen(path, "r");
    if (f) {                                  /* keep every line that is not one of ours */
        while (fgets(line, sizeof line, f)) {
            int ours = 0;
            for (unsigned i = 0; i < sizeof KEYS / sizeof *KEYS; i++) {
                size_t n = strlen(KEYS[i]);
                if (!strncmp(line, KEYS[i], n) && line[n] == '=') ours = 1;
            }
            size_t l = strlen(line);
            if (!ours && l > 1 && strlen(keep) + l + 1 < sizeof keep) {
                strcat(keep, line);
                if (line[l - 1] != '\n') strcat(keep, "\n");
            }
        }
        fclose(f);
    }
    f = fopen(path, "w");
    if (!f) { logf_("menu: cannot write %s\n", path); return; }
    fputs(keep, f);
    fprintf(f, "widescreen=%d\nperformance=%d\nfps60=%d\nfps60_effects=%d\nlowlod=%d\nfps=%d\n",
            g_ws_next >= 0 ? g_ws_next : g_widescreen, mode_of() == 1, g_fps60, g_fx60, g_lowlod, g_show_fps);
    fclose(f);
    logf_("menu: settings saved (mode %d, fps60 %d, effects %d, lowlod %d, fps %d)\n", mode_of(), g_fps60, g_fx60,
          g_lowlod, g_show_fps);
}

static void toggle(int item, int dir)
{
    int was60 = g_fps60;
    switch (item) {
    case M_MODE:
        if (mode_of() == 1) { g_fps60 = 0; g_fx60 = 1; g_lowlod = 0; }   /* -> quality */
        else { g_fps60 = 1; g_fx60 = 0; g_lowlod = 1; }                   /* quality/custom -> performance */
        break;
    case M_FPS60: g_fps60 ^= 1; break;
    case M_FX: g_fx60 ^= 1; break;
    case M_LOD: g_lowlod ^= 1; break;
    case M_FPS: g_show_fps ^= 1; break;
    case M_WS: g_ws_next = (g_ws_next >= 0 ? g_ws_next : g_widescreen) ^ 1; break;
    }
    (void)dir;
    if (g_fps60 != was60) io_fps60_changed();
}

static void draw(uint32_t *base, int sel)
{
    static const char *const MODES[] = {"Quality (30 fps)", "Performance (60 fps)", "Custom"};
    char v[M_COUNT][40];
    snprintf(v[M_MODE], 40, "%s", MODES[mode_of()]);
    snprintf(v[M_FPS60], 40, "%s", g_fps60 ? "on" : "off");
    snprintf(v[M_FX], 40, "%s", g_fx60 ? "on" : "off");
    snprintf(v[M_LOD], 40, "%s", g_lowlod ? "low" : "normal");
    snprintf(v[M_FPS], 40, "%s", g_show_fps ? "on" : "off");
    int ws = g_ws_next >= 0 ? g_ws_next : g_widescreen;
    snprintf(v[M_WS], 40, "%s%s", ws ? "on" : "off", ws != g_widescreen ? "  (after restart)" : "");
    v[M_CLOSE][0] = 0;
    static const char *const NAMES[M_COUNT] = {"Mode", "60 fps", "Mirror + smoke at 60 fps", "Opponent car detail",
                                               "FPS / CPU counter", "Widescreen 16:9", "Close and save"};
    pspDebugScreenSetBase(base);
    pspDebugScreenSetBackColor(0xFF301000);
    pspDebugScreenSetTextColor(0xFFFFFFFF);
    int x0 = 8, y0 = 8;
    for (int y = y0 - 1; y <= y0 + M_COUNT + 6; y++) {     /* panel background */
        pspDebugScreenSetXY(x0 - 1, y);
        pspDebugScreenPrintData("                                              ", 46);
    }
    pspDebugScreenSetXY(x0, y0);
    pspDebugScreenPrintf("GT2 PSP - settings");
    for (int i = 0; i < M_COUNT; i++) {
        pspDebugScreenSetXY(x0, y0 + 2 + i);
        pspDebugScreenSetTextColor(i == sel ? 0xFF40FFFF : 0xFFFFFFFF);
        pspDebugScreenPrintf("%c %-25s %s", i == sel ? '>' : ' ', NAMES[i], v[i]);
    }
    pspDebugScreenSetTextColor(0xFFA0A0A0);
    pspDebugScreenSetXY(x0, y0 + M_COUNT + 3);
    pspDebugScreenPrintf("Up/Down select, X/Left/Right change");
    pspDebugScreenSetXY(x0, y0 + M_COUNT + 4);
    pspDebugScreenPrintf("O / START / L+R+SELECT: back to the game");
}

static uint32_t buttons(void)
{
    SceCtrlData p;
    sceCtrlPeekBufferPositive(&p, 1);
    return p.Buttons;
}

static void run(void)
{
    logf_("menu: open\n");
    g_spu_paused = 1;
    static int inited;
    void *top; int bw, fmt;
    sceDisplayGetFrameBuf(&top, &bw, &fmt, PSP_DISPLAY_SETBUF_NEXTFRAME);
    if (!inited) { pspDebugScreenInitEx((void *)((uintptr_t)top | 0x40000000u), PSP_DISPLAY_PIXEL_FORMAT_5551, 0); inited = 1; }
    pspDebugScreenClearLineDisable();
    while (buttons() & COMBO) sceDisplayWaitVblankStart();
    uint32_t prev = 0;
    int sel = 0;
    for (;;) {
        uint32_t b = buttons(), e = b & ~prev;
        prev = b;
        if ((b & COMBO) == COMBO || (e & (PSP_CTRL_CIRCLE | PSP_CTRL_START))) break;
        if (e & PSP_CTRL_UP) sel = (sel + M_COUNT - 1) % M_COUNT;
        if (e & PSP_CTRL_DOWN) sel = (sel + 1) % M_COUNT;
        if (e & (PSP_CTRL_CROSS | PSP_CTRL_LEFT | PSP_CTRL_RIGHT)) {
            if (sel == M_CLOSE) { if (e & PSP_CTRL_CROSS) break; }
            else toggle(sel, (e & PSP_CTRL_LEFT) ? -1 : 1);
        }
        sceDisplayGetFrameBuf(&top, &bw, &fmt, PSP_DISPLAY_SETBUF_NEXTFRAME);
        if (top) draw((uint32_t *)((uintptr_t)top | 0x40000000u), sel);
        sceDisplayWaitVblankStart();
    }
    while (buttons()) sceDisplayWaitVblankStart();   /* the game must not see the buttons that closed the menu */
    save_settings();
    g_spu_paused = 0;
    logf_("menu: closed\n");
}

/* game thread, once per vblank (core.c rt_poll) */
void menu_poll(void)
{
    if ((buttons() & COMBO) == COMBO) run();
}
