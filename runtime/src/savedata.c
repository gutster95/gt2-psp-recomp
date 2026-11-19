/* Memory card storage through the PSP's own save data utility (sceUtilitySavedata).
 *
 * The 128 KB card image is one PSP save (ms0:/PSP/SAVEDATA/GT2PSP00000CARD1/, file MEMCARD.BIN) with title, detail
 * text and an icon, so it shows up in the XMB under Saved Data and can be copied or deleted there. The auto modes
 * are used: no PSP dialog appears, GT2's own memory card screens stay the only UI.
 *  - Load: once at startup, blocking (before the game runs).
 *  - Save: asynchronously. GT2 writes the card sector by sector; once no sector has been written for a second the
 *    game is done, a snapshot of the image is saved in the background (savedata_tick, every vblank). Writes during
 *    a running save simply schedule the next one. */
#include <string.h>
#include <pspkernel.h>
#include <pspdisplay.h>
#include <psputility.h>
#include "rt.h"
#include "savedata.h"
#include "save_icon.h"

#define GAME_NAME "GT2PSP000"     /* 9 characters like a product code */
#define SAVE_NAME "00CARD1"
#define FILE_NAME "MEMCARD.BIN"

static SceUtilitySavedataParam g_sp;
static uint8_t g_snapshot[SAVEDATA_CARD_SIZE] __attribute__((aligned(64)));
static int g_busy, g_dirty;
static uint64_t g_last_write;

static uint32_t checksum(const uint8_t *b)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < SAVEDATA_CARD_SIZE; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

static void setup(int mode, void *buf)
{
    memset(&g_sp, 0, sizeof g_sp);
    /* the parameter block without the fields of firmware 2.00+ (key, secureVersion, ...): the real PSP answered the
     * full size (1536 bytes, pspsdk with _PSP_FW_VERSION >= 200) with 0x80110388 (bad parameter) for every save,
     * PPSSPP accepted it; tested on the PSP-3000 with settings variants */
    g_sp.base.size = (unsigned)((char *)&g_sp.utilityData - (char *)&g_sp) + sizeof g_sp.utilityData;
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE, &g_sp.base.language);
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_UNKNOWN, &g_sp.base.buttonSwap);
    g_sp.base.graphicsThread = 0x11;
    g_sp.base.accessThread = 0x13;
    g_sp.base.fontThread = 0x12;
    g_sp.base.soundThread = 0x10;
    g_sp.mode = (PspUtilitySavedataMode)mode;
    g_sp.overwrite = 1;
    strcpy(g_sp.gameName, GAME_NAME);
    strcpy(g_sp.saveName, SAVE_NAME);
    strcpy(g_sp.fileName, FILE_NAME);
    g_sp.dataBuf = buf;
    g_sp.dataBufSize = SAVEDATA_CARD_SIZE;
    g_sp.dataSize = SAVEDATA_CARD_SIZE;
    strcpy(g_sp.sfoParam.title, "Gran Turismo 2 (PSP)");
    strcpy(g_sp.sfoParam.savedataTitle, "Memory Card 1");
    strcpy(g_sp.sfoParam.detail, "PlayStation memory card (slot 1) of the GT2 PSP port: GT mode progress, garage, "
                                  "licenses and settings.");
    g_sp.sfoParam.parentalLevel = 1;
    g_sp.icon0FileData.buf = (void *)g_save_icon;
    g_sp.icon0FileData.bufSize = sizeof g_save_icon;
    g_sp.icon0FileData.size = sizeof g_save_icon;
    g_sp.focus = PSP_UTILITY_SAVEDATA_FOCUS_LATEST;
}

int savedata_load(uint8_t *card)
{
    setup(PSP_UTILITY_SAVEDATA_AUTOLOAD, card);
    if (sceUtilitySavedataInitStart(&g_sp) < 0) { logf_("savedata: InitStart failed\n"); return 0; }
    for (int guard = 0; guard < 60 * 30; guard++) {           /* at most ~30 s */
        int st = sceUtilitySavedataGetStatus();
        if (st == PSP_UTILITY_DIALOG_VISIBLE) sceUtilitySavedataUpdate(1);
        else if (st == PSP_UTILITY_DIALOG_QUIT) sceUtilitySavedataShutdownStart();
        else if (st == PSP_UTILITY_DIALOG_NONE) break;
        sceDisplayWaitVblankStart();
    }
    logf_("savedata: load result %08x, card checksum %08x\n", (unsigned)g_sp.base.result, (unsigned)checksum(card));
    return g_sp.base.result == 0;
}

void savedata_written(void)
{
    g_dirty = 1;
    g_last_write = now_us();
}

void savedata_tick(const uint8_t *card)
{
    if (g_busy) {
        int st = sceUtilitySavedataGetStatus();
        if (st == PSP_UTILITY_DIALOG_VISIBLE) sceUtilitySavedataUpdate(1);
        else if (st == PSP_UTILITY_DIALOG_QUIT) sceUtilitySavedataShutdownStart();
        else if (st == PSP_UTILITY_DIALOG_NONE) {
            g_busy = 0;
            logf_("savedata: save result %08x\n", (unsigned)g_sp.base.result);
        }
        return;
    }
    if (g_dirty && now_us() - g_last_write > 1000000) {       /* the game finished writing */
        memcpy(g_snapshot, card, SAVEDATA_CARD_SIZE);
        logf_("savedata: saving card, checksum %08x\n", (unsigned)checksum(g_snapshot));
        setup(PSP_UTILITY_SAVEDATA_AUTOSAVE, g_snapshot);
        if (sceUtilitySavedataInitStart(&g_sp) < 0) { logf_("savedata: save InitStart failed\n"); return; }
        g_dirty = 0;
        g_busy = 1;
    }
}
