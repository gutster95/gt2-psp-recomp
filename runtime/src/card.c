/* Memory card in slot 1: a 128 KB card image (standard raw .mcd layout, 1024 sectors of 128 bytes) kept in RAM and
 * stored as a PSP save through the system's save data utility (savedata.c): it appears in the XMB under Saved
 * Data. A missing save means a freshly formatted card. An old GT2CARD1.MCD next to the EBOOT (earlier builds) is
 * taken over once.
 *
 * GT2 only uses the BIOS's raw sector functions and builds its own file handling on top:
 *   _card_info(port)               A0:AB  -> SwCARD event (IOE: card present, TIMEOUT: no card)
 *   _card_read(port, sector, dst)  B0:4F  -> HwCARD event when done
 *   _card_write(port, sector, src) B0:4E  -> HwCARD event when done (src = 0: "_new_card" acknowledge)
 * The results are delivered like the BIOS does - as events whose callbacks run in interrupt context - a little
 * later (from rt_poll), never inside the call itself. Slot 2 has no card. */
#include <stdio.h>
#include <string.h>
#include "rt.h"
#include "card.h"
#include "savedata.h"

#define SECTOR 128
#define SECTORS 1024
#define HW_CARD 0xF0000011u
#define SW_CARD 0xF4000001u
#define EV_IOE 0x0004u
#define EV_TIMEOUT 0x0100u

static uint8_t g_img[SECTOR * SECTORS];
static int g_present;
static char g_path[320];

static void checksum(uint8_t *f)
{
    uint8_t x = 0;
    for (int i = 0; i < 127; i++) x ^= f[i];
    f[127] = x;
}

/* Freshly formatted card (nocash psx-spx "Memory Card Data Format") */
static void format(void)
{
    memset(g_img, 0, sizeof g_img);
    uint8_t *f = g_img;
    f[0] = 'M'; f[1] = 'C';
    checksum(f);
    for (int i = 1; i < 16; i++) {                 /* directory frames: free */
        f = g_img + i * SECTOR;
        f[0] = 0xA0;
        f[8] = f[9] = 0xFF;
        checksum(f);
    }
    for (int i = 16; i < 36; i++) {                /* broken sector list: none */
        f = g_img + i * SECTOR;
        memset(f, 0xFF, 4);
        f[8] = f[9] = 0xFF;
        checksum(f);
    }
    memcpy(g_img + 63 * SECTOR, g_img, SECTOR);    /* write test frame */
}

void card_init(void)
{
    g_present = 1;
    if (savedata_load(g_img)) { logf_("card: loaded from PSP save data\n"); return; }
    snprintf(g_path, sizeof g_path, "%sGT2CARD1.MCD", g_basedir);   /* card file of earlier builds */
    FILE *f = fopen(g_path, "rb");
    if (f) {
        size_t n = fread(g_img, 1, sizeof g_img, f);
        fclose(f);
        if (n == sizeof g_img) {
            logf_("card: took over %s, it is saved as PSP save data from now on (the file can be deleted)\n", g_path);
            savedata_written();
            return;
        }
    }
    format();
    logf_("card: no save yet - new formatted card\n");
}

void card_tick(void) { savedata_tick(g_img); }

static int slot1(uint32_t port) { return (port & 0x30) == 0 && g_present; }

static int g_log_ops = 64;   /* log the first card operations */

uint32_t card_info(uint32_t port)
{
    if (g_log_ops > 0) { g_log_ops--; logf_("card: info(port %02x)\n", (unsigned)port); }
    bios_event_later(SW_CARD, slot1(port) ? EV_IOE : EV_TIMEOUT);
    return 1;
}

uint32_t card_read(uint32_t port, uint32_t sector, uint32_t dst)
{
    if (g_log_ops > 0) { g_log_ops--; logf_("card: read(port %02x, sector %u)\n", (unsigned)port, (unsigned)sector); }
    if (!slot1(port) || sector >= SECTORS) { bios_event_later(HW_CARD, EV_TIMEOUT); return 1; }
    for (int i = 0; i < SECTOR; i++) mem_w8(dst + i, g_img[sector * SECTOR + i]);
    bios_event_later(HW_CARD, EV_IOE);
    return 1;
}

uint32_t card_write(uint32_t port, uint32_t sector, uint32_t src)
{
    if (g_log_ops > 0) { g_log_ops--; logf_("card: write(port %02x, sector %u, src %08x)\n", (unsigned)port, (unsigned)sector, (unsigned)src); }
    if (!slot1(port) || sector >= SECTORS) { bios_event_later(HW_CARD, EV_TIMEOUT); return 1; }
    if (src) {                                     /* src = 0: _new_card() acknowledge, nothing is written */
        for (int i = 0; i < SECTOR; i++) g_img[sector * SECTOR + i] = (uint8_t)mem_r8(src + i);
        savedata_written();
    }
    bios_event_later(HW_CARD, EV_IOE);
    return 1;
}
