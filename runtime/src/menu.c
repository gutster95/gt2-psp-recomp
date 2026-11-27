/* In-game settings menu: L + R + SELECT opens it anywhere (title, menus, race). The game thread stops inside rt_poll
 * while the menu is open (the game is paused, SPU output muted); the menu is drawn with pspdebug's 8x8 font into the
 * picture on screen. Every option takes effect at once; closing writes them to settings.txt.
 * The picture mode (30 / 60 fps) is chosen in the launcher before the game starts and only shown here; 16:9 is on by
 * default (settings.txt widescreen=0 for 4:3). Switching 30 -> 60 fps during a race ran the game at double speed. */
#include <stdio.h>
#include <string.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspdebug.h>
#include <pspkernel.h>
#include "rt.h"

extern int g_fps60, g_fx60, g_lowlod, g_show_fps, g_lr_shift, g_race_buttons;
extern volatile int g_spu_paused;

#define COMBO (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER | PSP_CTRL_SELECT)

enum { M_FX, M_LOD, M_FPS, M_LR, M_RB, M_CLOSE, M_COUNT };

/* Only the menu's own keys are rewritten; every other line of settings.txt (mode, 16:9, test options) is kept. */
static void save_settings(void)
{
    static const char *const KEYS[] = {"fps60_effects", "lowlod", "fps", "lr_shift", "race_buttons"};
    char path[300], keep[2048] = "", line[256];
    snprintf(path, sizeof path, "%ssettings.txt", g_basedir);
    FILE *f = fopen(path, "r");
    if (f) {
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
    fprintf(f, "fps60_effects=%d\nlowlod=%d\nfps=%d\nlr_shift=%d\nrace_buttons=%d\n", g_fx60, g_lowlod, g_show_fps,
            g_lr_shift, g_race_buttons);
    fclose(f);
    logf_("menu: settings saved (effects %d, lowlod %d, fps %d, lr_shift %d, race_buttons %d)\n", g_fx60, g_lowlod,
          g_show_fps, g_lr_shift, g_race_buttons);
}

static void toggle(int item)
{
    switch (item) {
    case M_FX: g_fx60 ^= 1; break;
    case M_LOD: g_lowlod ^= 1; break;
    case M_FPS: g_show_fps ^= 1; break;
    case M_LR: g_lr_shift ^= 1; break;
    case M_RB: g_race_buttons ^= 1; break;
    }
}

static void draw(uint32_t *base, int sel)
{
    char v[M_COUNT][40];
    snprintf(v[M_FX], 40, "%s", g_fx60 ? "on" : "off");
    snprintf(v[M_LOD], 40, "%s", g_lowlod ? "low" : "normal");
    snprintf(v[M_FPS], 40, "%s", g_show_fps ? "on" : "off");
    snprintf(v[M_LR], 40, "%s", g_lr_shift ? "shift (L2/R2)" : "views (L1/R1)");
    snprintf(v[M_RB], 40, "%s", g_race_buttons ? "X gas, square brake" : "as the game");
    v[M_CLOSE][0] = 0;
    static const char *const NAMES[M_COUNT] = {"Mirror + smoke", "Opponent car detail", "FPS / CPU counter",
                                               "L / R buttons", "Race buttons", "Close and save"};
    pspDebugScreenSetBase(base);
    pspDebugScreenSetBackColor(0xFF301000);
    pspDebugScreenSetTextColor(0xFFFFFFFF);
    int x0 = 8, y0 = 8;
    for (int y = y0 - 1; y <= y0 + M_COUNT + 7; y++) {     /* panel background */
        pspDebugScreenSetXY(x0 - 1, y);
        pspDebugScreenPrintData("                                              ", 46);
    }
    pspDebugScreenSetXY(x0, y0);
    pspDebugScreenPrintf("GT2 PSP - Settings");
    pspDebugScreenSetXY(x0, y0 + 1);
    pspDebugScreenSetTextColor(0xFFA0A0A0);
    pspDebugScreenPrintf("Mode: %s", g_fps60 ? "Performance (60 fps)" : "Quality (30 fps)");
    for (int i = 0; i < M_COUNT; i++) {
        pspDebugScreenSetXY(x0, y0 + 3 + i);
        pspDebugScreenSetTextColor(i == sel ? 0xFF40FFFF : 0xFFFFFFFF);
        pspDebugScreenPrintf("%c %-22s %s", i == sel ? '>' : ' ', NAMES[i], v[i]);
    }
    pspDebugScreenSetTextColor(0xFFA0A0A0);
    pspDebugScreenSetXY(x0, y0 + M_COUNT + 4);
    pspDebugScreenPrintf("Up/Down select, X/Left/Right change");
    pspDebugScreenSetXY(x0, y0 + M_COUNT + 5);
    pspDebugScreenPrintf("Mode: in the start menu");
    pspDebugScreenSetXY(x0, y0 + M_COUNT + 6);
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
            else toggle(sel);
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
