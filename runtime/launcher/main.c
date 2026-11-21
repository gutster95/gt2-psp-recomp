/* GT2 launcher (EBOOT.PBP): choose Simulation or Arcade Mode, then start that disc's runtime build - GT2SIM.PRX or
 * GT2ARC.PRX next to the EBOOT, each a complete recompiled game (runtime/CMakeLists.txt GT2_GAME). The PRX is
 * loaded and started like PSPLink starts it; argv[0] is its path (the runtime takes its base directory from it).
 * A disc is offered when its PRX and its data pack (GT2.PAK / GT2A.PAK) are there; the last choice is preselected
 * (lastgame.txt). */
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <string.h>

PSP_MODULE_INFO("GT2Launcher", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(256);

static char g_dir[256];

static int exists(const char *name)
{
    char p[320];
    SceIoStat st;
    snprintf(p, sizeof p, "%s%s", g_dir, name);
    return sceIoGetstat(p, &st) >= 0;
}

static int last_game(void)
{
    char p[320], c = 0;
    snprintf(p, sizeof p, "%slastgame.txt", g_dir);
    SceUID fd = sceIoOpen(p, PSP_O_RDONLY, 0);
    if (fd < 0) return 0;
    sceIoRead(fd, &c, 1);
    sceIoClose(fd);
    return c == 'a';
}

static void save_game(int sel)
{
    char p[320];
    snprintf(p, sizeof p, "%slastgame.txt", g_dir);
    SceUID fd = sceIoOpen(p, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, sel ? "arc\n" : "sim\n", 4);
    sceIoClose(fd);
}

static void message(const char *text)
{
    pspDebugScreenInit();
    pspDebugScreenSetXY(4, 10);
    pspDebugScreenPrintf("%s", text);
    sceKernelDelayThread(5 * 1000 * 1000);
    sceKernelExitGame();
}

static int menu(void)
{
    static const char *const NAMES[2] = {"Simulation Mode", "Arcade Mode"};
    pspDebugScreenInit();
    pspDebugScreenClearLineDisable();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
    SceCtrlData pad;
    uint32_t prev = ~0u;                          /* buttons held at start do not count */
    int sel = last_game();
    for (int frame = 0;; frame++) {
        sceCtrlPeekBufferPositive(&pad, 1);
        uint32_t b = pad.Buttons, e = b & ~prev;
        prev = b;
        if (e & (PSP_CTRL_UP | PSP_CTRL_DOWN)) sel ^= 1;
        if (e & (PSP_CTRL_CROSS | PSP_CTRL_START)) break;
        if (frame == 0 || (e & (PSP_CTRL_UP | PSP_CTRL_DOWN))) {
            pspDebugScreenSetBackColor(0xFF000000);
            pspDebugScreenClear();
            pspDebugScreenSetXY(19, 10);
            pspDebugScreenSetTextColor(0xFFFFFFFF);
            pspDebugScreenPrintf("GRAN TURISMO 2");
            for (int i = 0; i < 2; i++) {
                pspDebugScreenSetXY(19, 14 + 2 * i);
                pspDebugScreenSetTextColor(i == sel ? 0xFF40FFFF : 0xFFA0A0A0);
                pspDebugScreenPrintf("%c %s", i == sel ? '>' : ' ', NAMES[i]);
            }
            pspDebugScreenSetXY(19, 21);
            pspDebugScreenSetTextColor(0xFF808080);
            pspDebugScreenPrintf("Up/Down, X: start");
        }
        sceDisplayWaitVblankStart();
    }
    for (int i = 0; i < 60; i++) {               /* the game must not see the X that started it */
        sceCtrlPeekBufferPositive(&pad, 1);
        if (!pad.Buttons) break;
        sceDisplayWaitVblankStart();
    }
    pspDebugScreenClear();
    return sel;
}

int main(int argc, char **argv)
{
    if (argc > 0 && argv[0]) {
        strncpy(g_dir, argv[0], sizeof g_dir - 1);
        char *s = strrchr(g_dir, '/');
        if (s) s[1] = 0; else g_dir[0] = 0;
    }
    int sim = exists("GT2SIM.PRX") && exists("GT2.PAK");
    int arc = exists("GT2ARC.PRX") && exists("GT2A.PAK");
    if (!sim && !arc) message("GT2: GT2SIM.PRX + GT2.PAK or GT2ARC.PRX + GT2A.PAK missing");
    int sel = sim && arc ? menu() : arc;
    if (sim && arc) save_game(sel);
    char path[320];
    snprintf(path, sizeof path, "%s%s", g_dir, sel ? "GT2ARC.PRX" : "GT2SIM.PRX");
    SceUID mod = sceKernelLoadModule(path, 0, NULL);
    if (mod < 0) message("GT2: cannot load the game module");
    int status;
    if (sceKernelStartModule(mod, strlen(path) + 1, path, &status, NULL) < 0) message("GT2: cannot start the game module");
    sceKernelExitDeleteThread(0);                 /* the game runs in its own module from here */
    return 0;
}
