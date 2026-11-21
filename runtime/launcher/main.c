/* GT2 launcher (EBOOT.PBP): choose Simulation or Arcade Mode, then start that disc's runtime build - GT2SIM.PRX or
 * GT2ARC.PRX next to the EBOOT, each a complete recompiled game (runtime/CMakeLists.txt GT2_GAME). The PRX is
 * loaded and started like PSPLink starts it; argv[0] is its path (the runtime takes its base directory from it).
 * A disc is offered when its PRX and its data pack (GT2.PAK / GT2A.PAK) are there; the last choice is preselected
 * (lastgame.txt). The picture mode (30 / 60 fps) is chosen here too and written to settings.txt. */
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

/* Picture mode (settings.txt, read by both runtimes at start): it cannot change during a race - switching 30 -> 60
 * fps there ran the game at double speed - so it is chosen here. */
static int read_mode(void)
{
    char p[320], line[128];
    int perf = 0;
    snprintf(p, sizeof p, "%ssettings.txt", g_dir);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, "performance=1", 13) || !strncmp(line, "fps60=1", 7)) perf = 1;
    fclose(f);
    return perf;
}

static void write_mode(int perf)
{
    static const char *const KEYS[] = {"performance=", "fps60=", "fps60_effects=", "lowlod="};
    char p[320], line[256], keep[4096] = "";
    snprintf(p, sizeof p, "%ssettings.txt", g_dir);
    FILE *f = fopen(p, "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            int ours = 0;
            for (int i = 0; i < 4; i++) if (!strncmp(line, KEYS[i], strlen(KEYS[i]))) ours = 1;
            size_t l = strlen(line);
            if (!ours && l > 1 && strlen(keep) + l + 2 < sizeof keep) {
                strcat(keep, line);
                if (line[l - 1] != '\n') strcat(keep, "\n");
            }
        }
        fclose(f);
    }
    f = fopen(p, "w");
    if (!f) return;
    fputs(keep, f);
    fputs(perf ? "performance=1\nfps60=1\nfps60_effects=0\nlowlod=1\n" : "performance=0\nfps60=0\nfps60_effects=1\nlowlod=0\n",
          f);
    fclose(f);
}

static void message(const char *text)
{
    pspDebugScreenInit();
    pspDebugScreenSetXY(4, 10);
    pspDebugScreenPrintf("%s", text);
    sceKernelDelayThread(5 * 1000 * 1000);
    sceKernelExitGame();
}

static int menu(const int have[2])
{
    static const char *const NAMES[2] = {"Simulation Mode", "Arcade Mode"};
    static const char *const MODES[2] = {"Quality      (30 fps)", "Performance  (60 fps)"};
    pspDebugScreenInit();
    pspDebugScreenClearLineDisable();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
    SceCtrlData pad;
    uint32_t prev = ~0u;                          /* buttons held at start do not count */
    int sel = have[1] && (last_game() || !have[0]) ? 1 : 0;
    int perf = read_mode();
    for (int frame = 0;; frame++) {
        sceCtrlPeekBufferPositive(&pad, 1);
        uint32_t b = pad.Buttons, e = b & ~prev;
        prev = b;
        int moved = 0;
        if ((e & (PSP_CTRL_UP | PSP_CTRL_DOWN)) && have[0] && have[1]) { sel ^= 1; moved = 1; }
        if (e & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT)) { perf ^= 1; moved = 1; }
        if (e & (PSP_CTRL_CROSS | PSP_CTRL_START)) break;
        if (frame == 0 || moved) {
            pspDebugScreenSetBackColor(0xFF000000);
            pspDebugScreenClear();
            pspDebugScreenSetXY(19, 8);
            pspDebugScreenSetTextColor(0xFFFFFFFF);
            pspDebugScreenPrintf("GRAN TURISMO 2");
            for (int i = 0; i < 2; i++) {
                pspDebugScreenSetXY(19, 12 + 2 * i);
                pspDebugScreenSetTextColor(!have[i] ? 0xFF404040 : i == sel ? 0xFF40FFFF : 0xFFA0A0A0);
                pspDebugScreenPrintf("%c %s", i == sel ? '>' : ' ', NAMES[i]);
            }
            pspDebugScreenSetXY(19, 18);
            pspDebugScreenSetTextColor(0xFF40FFFF);
            pspDebugScreenPrintf("< %s >", MODES[perf]);
            pspDebugScreenSetXY(19, 23);
            pspDebugScreenSetTextColor(0xFF808080);
            pspDebugScreenPrintf("Up/Down: game   Left/Right: mode");
            pspDebugScreenSetXY(19, 24);
            pspDebugScreenPrintf("X: start");
        }
        sceDisplayWaitVblankStart();
    }
    write_mode(perf);
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
    const int have[2] = {sim, arc};
    int sel = menu(have);                         /* game (when both are there) and picture mode */
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
