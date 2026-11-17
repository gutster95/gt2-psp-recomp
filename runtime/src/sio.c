/* SIO0: controller and memory card port (0x1F801040-0x1F80104F), nocash psx-spx protocol.
 * Port 1: digital pad fed from the PSP buttons. Port 2 and memory cards: not connected (no /ACK). */
#include <stdio.h>
#include <string.h>
#include <pspctrl.h>
#include "rt.h"
#include "sio.h"

static uint16_t g_ctrl, g_mode, g_baud;
static uint8_t g_rx;
static int g_rx_full;
static int g_irq;              /* JOY_STAT bit 9 */
static int g_step;             /* byte index within the current transfer */
static int g_device;           /* 0 none, 1 pad, 2 memory card (absent) */
static uint8_t g_btn_lo = 0xFF, g_btn_hi = 0xFF;

/* Scripted input for testing: <basedir>/input.txt, lines "<vblank> <button> <duration-in-vblanks>".
 * Buttons: start select up down left right cross circle square triangle l1 r1 l2 r2. */
typedef struct { uint32_t at, len; uint16_t bits; } Inject;   /* bits: lo | hi << 8 (active high) */
static Inject g_inject[64];
static int g_ninject;

static void load_inject(void)
{
    static const struct { const char *name; uint16_t bits; } names[] = {
        {"select", 0x01}, {"start", 0x08}, {"up", 0x10}, {"right", 0x20}, {"down", 0x40}, {"left", 0x80},
        {"l2", 0x100}, {"r2", 0x200}, {"l1", 0x400}, {"r1", 0x800}, {"triangle", 0x1000},
        {"circle", 0x2000}, {"cross", 0x4000}, {"square", 0x8000},
    };
    char path[320], name[16];
    snprintf(path, sizeof path, "%sinput.txt", g_basedir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    unsigned at, len;
    while (g_ninject < 64 && fscanf(f, "%u %15s %u", &at, name, &len) == 3) {
        for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
            if (!strcmp(name, names[i].name)) {
                g_inject[g_ninject++] = (Inject){at, len, names[i].bits};
                break;
            }
    }
    fclose(f);
    logf_("input.txt: %d scripted presses\n", g_ninject);
}

void sio_init(void)
{
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
    load_inject();
}

/* PS1 digital pad bits (active low):
 * lo: 0 Select, 1 L3, 2 R3, 3 Start, 4 Up, 5 Right, 6 Down, 7 Left
 * hi: 0 L2, 1 R2, 2 L1, 3 R1, 4 Triangle, 5 Circle, 6 Cross, 7 Square */
static void sample_buttons(void)
{
    SceCtrlData d;
    sceCtrlPeekBufferPositive(&d, 1);
    uint32_t b = d.Buttons;
    uint8_t lo = 0, hi = 0;
    if (b & PSP_CTRL_SELECT) lo |= 0x01;
    if (b & PSP_CTRL_START) lo |= 0x08;
    if (b & PSP_CTRL_UP) lo |= 0x10;
    if (b & PSP_CTRL_RIGHT) lo |= 0x20;
    if (b & PSP_CTRL_DOWN) lo |= 0x40;
    if (b & PSP_CTRL_LEFT) lo |= 0x80;
    if (b & PSP_CTRL_LTRIGGER) hi |= 0x04;
    if (b & PSP_CTRL_RTRIGGER) hi |= 0x08;
    if (b & PSP_CTRL_TRIANGLE) hi |= 0x10;
    if (b & PSP_CTRL_CIRCLE) hi |= 0x20;
    if (b & PSP_CTRL_CROSS) hi |= 0x40;
    if (b & PSP_CTRL_SQUARE) hi |= 0x80;
    /* analog stick as d-pad */
    if (d.Lx < 64) lo |= 0x80;
    if (d.Lx > 192) lo |= 0x20;
    if (d.Ly < 64) lo |= 0x10;
    if (d.Ly > 192) lo |= 0x40;
    for (int i = 0; i < g_ninject; i++)
        if (g_vblank_count >= g_inject[i].at && g_vblank_count < g_inject[i].at + g_inject[i].len) {
            lo |= (uint8_t)g_inject[i].bits;
            hi |= (uint8_t)(g_inject[i].bits >> 8);
        }
    g_btn_lo = (uint8_t)~lo;
    g_btn_hi = (uint8_t)~hi;
}

static void transfer(uint8_t tx)
{
    uint8_t rx = 0xFF;
    int ack = 0;
    int port = (g_ctrl >> 13) & 1;
    if (g_step == 0) {
        g_device = 0;
        if (tx == 0x01 && port == 0) { g_device = 1; ack = 1; }
        else if (tx == 0x81) g_device = 2;              /* memory card: absent, no ack */
    } else if (g_device == 1) {
        switch (g_step) {
        case 1: rx = 0x41; ack = 1; if (tx == 0x42) sample_buttons(); break;
        case 2: rx = 0x5A; ack = 1; break;
        case 3: rx = g_btn_lo; ack = 1; break;
        case 4: rx = g_btn_hi; ack = 0; break;
        }
    }
    g_step++;
    g_rx = rx;
    g_rx_full = 1;
    if (ack) {
        if (g_ctrl & 0x1000) { g_irq = 1; raise_irq(7); }
    } else {
        g_device = 0;
    }
}

uint32_t sio_read(uint32_t off, int size)
{
    (void)size;
    switch (off) {
    case 0x0: { uint8_t v = g_rx_full ? g_rx : 0xFF; g_rx_full = 0; return v; }
    case 0x4: return 0x5 | (g_rx_full ? 0x2 : 0) | (g_irq ? 0x200 : 0);    /* JOY_STAT */
    case 0x8: return g_mode;
    case 0xA: return g_ctrl;
    case 0xE: return g_baud;
    }
    return 0;
}

void sio_write(uint32_t off, uint32_t v, int size)
{
    (void)size;
    switch (off) {
    case 0x0: transfer((uint8_t)v); break;
    case 0x8: g_mode = (uint16_t)v; break;
    case 0xA:
        if (v & 0x40) { g_ctrl = 0; g_irq = 0; g_rx_full = 0; g_step = 0; break; }   /* reset */
        if (v & 0x10) g_irq = 0;                                                    /* acknowledge */
        if (!(v & 0x2)) g_step = 0;                                                 /* deselect */
        else if (!(g_ctrl & 0x2)) g_step = 0;                                       /* new select */
        g_ctrl = (uint16_t)(v & ~0x50);
        break;
    case 0xE: g_baud = (uint16_t)v; break;
    }
}
