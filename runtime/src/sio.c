/* SIO0: controller port (0x1F801040-0x1F80104F), nocash psx-spx protocol.
 * Port 1: DualShock (SCPH-1200) fed from the PSP controls, starting in analog mode (settings.txt analog=0: digital
 * pad). The PSP stick is the left analog stick (with a dead zone); the right stick stays centred. GT2 drives the
 * pad itself, including the DualShock config commands (0x43 config mode, 0x44 analog/digital, 0x45-0x4D queries).
 * PSP mapping: L/R = L1/R1, SELECT+L / SELECT+R = L2/R2. Port 2: no pad. The memory card is handled at the BIOS
 * level (card.c), so card accesses on SIO (0x81) see no device. */
#include <stdio.h>
#include <string.h>
#include <pspctrl.h>
#include "rt.h"
#include "sio.h"
#include "game.h"
#ifndef GT2_RACE_BUTTONS   /* game.h of builds generated before 2026-11-27 */
#define GT2_RACE_BUTTONS 1
#endif

static uint16_t g_ctrl, g_mode, g_baud;
static uint8_t g_rx;
static int g_rx_full;
static int g_irq;              /* JOY_STAT bit 9 */
static int g_ack_pending;      /* /ACK of the last byte, signalled when its response is read */
static int g_step;             /* byte index within the current transfer */
static int g_device;           /* 0 none, 1 pad, 2 memory card (absent) */
static uint8_t g_btn_lo = 0xFF, g_btn_hi = 0xFF;
static uint8_t g_lx = 0x80, g_ly = 0x80;          /* left stick */
int g_pad_analog = 1;          /* analog (DualShock) mode; settings.txt analog */
/* settings.txt lr_shift (menu): 1 = PSP L/R are PS1 L2/R2 (GT2: down/upshift) and SELECT+L/R are L1/R1 (rear view,
 * change view); 0 = the other way round */
int g_lr_shift = 0;
int g_dpad_shift = 1;          /* settings.txt dpad_shift: d-pad up/down shift gears in races */
/* settings.txt race_buttons (default on): in races the face buttons are turned so that X accelerates, square brakes,
 * circle is the handbrake and triangle reverse (GT2's manual layout). The game's own button setup on the port
 * (DualShock analog mode with the user's save) is circle gas, triangle brake, X handbrake, square reverse. */
int g_race_buttons = GT2_RACE_BUTTONS;   /* game.h: off (and not offered) on the EU / JP builds */
static int g_config;           /* DualShock config mode */
static uint8_t g_cmd;          /* command byte of the current transfer */
static uint8_t g_resp[10];     /* response bytes from index 2 on */
static int g_resp_len;         /* number of bytes after the ID byte */
static uint8_t g_tx3;          /* first parameter byte (index 3) */

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
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
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
    int shift = (b & PSP_CTRL_SELECT) && (b & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER));
    if (shift) lo &= ~0x01;                        /* SELECT used as modifier: L2/R2 */
    if (g_lr_shift) shift = !shift;
    if (b & PSP_CTRL_LTRIGGER) hi |= shift ? 0x01 : 0x04;
    if (b & PSP_CTRL_RTRIGGER) hi |= shift ? 0x02 : 0x08;
    {   /* settings.txt dpad_shift: in races d-pad up/down also press R2/L2 (GT2: upshift/downshift). The d-pad bits stay,
         * so the pause menu still navigates; outside races (menus) nothing changes. */
        extern int ge_in_race(void);
        if (g_dpad_shift && (b & (PSP_CTRL_UP | PSP_CTRL_DOWN)) && ge_in_race()) {
            if (b & PSP_CTRL_UP) hi |= 0x02;
            if (b & PSP_CTRL_DOWN) hi |= 0x01;
        }
    }
    extern int ge_in_race(void);
    if (GT2_RACE_BUTTONS && g_race_buttons && ge_in_race()) {          /* PSP X -> PS circle, square -> triangle, circle -> X, triangle -> square */
        if (b & PSP_CTRL_TRIANGLE) hi |= 0x80;
        if (b & PSP_CTRL_CIRCLE) hi |= 0x40;
        if (b & PSP_CTRL_CROSS) hi |= 0x20;
        if (b & PSP_CTRL_SQUARE) hi |= 0x10;
    } else {
        if (b & PSP_CTRL_TRIANGLE) hi |= 0x10;
        if (b & PSP_CTRL_CIRCLE) hi |= 0x20;
        if (b & PSP_CTRL_CROSS) hi |= 0x40;
        if (b & PSP_CTRL_SQUARE) hi |= 0x80;
    }
    if (g_pad_analog) {
        /* left analog stick with a dead zone (the PSP nub rarely rests exactly at the centre) */
        int ax = d.Lx - 128, ay = d.Ly - 128;
        const int dz = 24;
        ax = ax > dz ? (ax - dz) * 127 / (127 - dz) : ax < -dz ? (ax + dz) * 128 / (128 - dz) : 0;
        ay = ay > dz ? (ay - dz) * 127 / (127 - dz) : ay < -dz ? (ay + dz) * 128 / (128 - dz) : 0;
        g_lx = (uint8_t)(128 + (ax > 127 ? 127 : ax < -128 ? -128 : ax));
        g_ly = (uint8_t)(128 + (ay > 127 ? 127 : ay < -128 ? -128 : ay));
    } else {
        /* digital pad: the stick works as d-pad */
        if (d.Lx < 64) lo |= 0x80;
        if (d.Lx > 192) lo |= 0x20;
        if (d.Ly < 64) lo |= 0x10;
        if (d.Ly > 192) lo |= 0x40;
    }
    for (int i = 0; i < g_ninject; i++)
        if (g_vblank_count >= g_inject[i].at && g_vblank_count < g_inject[i].at + g_inject[i].len) {
            lo |= (uint8_t)g_inject[i].bits;
            hi |= (uint8_t)(g_inject[i].bits >> 8);
        }
    g_btn_lo = (uint8_t)~lo;
    g_btn_hi = (uint8_t)~hi;
}

/* Response after the ID byte for command g_cmd (index 2: 0x5A, then data). */
static void build_response(void)
{
    static const uint8_t zero6[6] = {0};
    uint8_t *r = g_resp;
    r[0] = 0x5A;
    if (g_config) {
        g_resp_len = 7;
        memcpy(r + 1, zero6, 6);
        switch (g_cmd) {
        case 0x45: { uint8_t m[6] = {0x03, 0x02, (uint8_t)(g_pad_analog ? 1 : 0), 0x02, 0x01, 0x00}; memcpy(r + 1, m, 6); break; }
        case 0x47: { uint8_t m[6] = {0x00, 0x00, 0x02, 0x00, 0x01, 0x00}; memcpy(r + 1, m, 6); break; }
        case 0x4D: memset(r + 1, 0xFF, 6); break;
        case 0x42: case 0x43:
            r[1] = g_btn_lo; r[2] = g_btn_hi; r[3] = 0x80; r[4] = 0x80; r[5] = g_lx; r[6] = g_ly;
            break;
        default: break;                             /* 0x44, 0x46, 0x4C: zeros / filled after the parameter */
        }
        return;
    }
    r[1] = g_btn_lo;
    r[2] = g_btn_hi;
    if (g_pad_analog) {
        r[3] = 0x80; r[4] = 0x80;                   /* right stick: centred */
        r[5] = g_lx; r[6] = g_ly;
        g_resp_len = 7;
    } else {
        g_resp_len = 3;
    }
}

static void transfer(uint8_t tx)
{
    uint8_t rx = 0xFF;
    int ack = 0;
    int port = (g_ctrl >> 13) & 1;
    if (g_step == 0) {
        g_device = 0;
        if (tx == 0x01 && port == 0) { g_device = 1; ack = 1; }
    } else if (g_device == 1) {
        if (g_step == 1) {
            g_cmd = tx;
            if (tx == 0x42 || tx == 0x43) sample_buttons();
            rx = g_config ? 0xF3 : g_pad_analog ? 0x73 : 0x41;
            if (!g_config && tx != 0x42 && tx != 0x43) { g_device = 0; rx = 0xFF; }   /* config cmds need config mode */
            else { build_response(); ack = 1; }
        } else {
            int i = g_step - 2;
            if (g_step == 3) {
                g_tx3 = tx;
                if (g_config && g_cmd == 0x46) {
                    uint8_t m[2][5] = {{0x00, 0x01, 0x02, 0x00, 0x0A}, {0x00, 0x01, 0x01, 0x01, 0x14}};
                    memcpy(g_resp + 2, m[tx & 1], 5);
                } else if (g_config && g_cmd == 0x4C) {
                    g_resp[4] = (tx & 1) ? 0x07 : 0x04;
                }
            }
            if (g_step == 4 && g_config && g_cmd == 0x44) {}   /* tx = lock flag: ignored */
            rx = i < g_resp_len ? g_resp[i] : 0xFF;
            ack = i < g_resp_len - 1;
            if (i == g_resp_len - 1) {              /* last byte: apply the command */
                if (g_cmd == 0x43) {
                    int enter = g_tx3 == 1;
                    if (enter != g_config) logf_("pad: %s config mode\n", enter ? "enter" : "leave");
                    g_config = enter;
                } else if (g_config && g_cmd == 0x44) {
                    if ((g_tx3 == 1) != g_pad_analog) logf_("pad: %s mode\n", g_tx3 == 1 ? "analog" : "digital");
                    g_pad_analog = g_tx3 == 1;
                }
            }
        }
    }
    g_step++;
    g_rx = rx;
    g_rx_full = 1;
    /* The device's /ACK pulse comes after the byte: libpad clears IRQ7, waits for RX, reads it and only then
     * waits for the ACK interrupt - so the interrupt is raised when the received byte is read. */
    if (ack) g_ack_pending = 1;
    else g_device = 0;
}

uint32_t sio_read(uint32_t off, int size)
{
    (void)size;
    switch (off) {
    case 0x0: {
        uint8_t v = g_rx_full ? g_rx : 0xFF;
        g_rx_full = 0;
        if (g_ack_pending) {
            g_ack_pending = 0;
            if (g_ctrl & 0x1000) { g_irq = 1; raise_irq(7); }
        }
        return v;
    }
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
        if (v & 0x40) { g_ctrl = 0; g_irq = 0; g_rx_full = 0; g_step = 0; g_ack_pending = 0; break; }   /* reset */
        if (v & 0x10) g_irq = 0;                                                    /* acknowledge */
        if (!(v & 0x2)) g_step = 0;                                                 /* deselect */
        else if (!(g_ctrl & 0x2)) g_step = 0;                                       /* new select */
        g_ctrl = (uint16_t)(v & ~0x50);
        break;
    case 0xE: g_baud = (uint16_t)v; break;
    }
}
