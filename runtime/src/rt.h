/* Runtime-internal declarations. */
#pragma once
#include <setjmp.h>
#include <stdint.h>
#include "gt2rt.h"

#define SENTINEL_RA 0xFFFFFFF0u   /* return address given to the program entry */

/* log.c */
void log_init(const char *dir);
void log_close(void);
void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void fatal(PsxCpu *c, const char *fmt, ...) __attribute__((format(printf, 2, 3), noreturn));
void dump_trace(void);
extern char g_basedir[256];

/* core.c */
extern int g_cur_overlay;             /* -1: boot code mapped, 0-5: overlay index */
RecompFunc lookup(uint32_t addr);
void run_program(PsxCpu *c);
void raise_irq(int bit);
void irq_abandon(void);
void rt_callback(PsxCpu *c, uint32_t func);   /* call game code like an interrupt handler */
uint64_t now_us(void);

/* bios.c */
void bios_call(PsxCpu *c, uint32_t table);
extern uint32_t g_hook_entry_int;     /* PS1 address of the HookEntryInt buffer, 0 if none */
extern uint32_t g_irq_chain[4];       /* SysEnqIntRP queues (priority 0-3), PS1 addresses of first entry */
extern int g_irq_enabled;             /* EnterCriticalSection/ExitCriticalSection state */
void bios_return_from_exception(PsxCpu *c) __attribute__((noreturn));
void bios_poll_events(PsxCpu *c);

/* io.c */
extern uint32_t g_i_stat, g_i_mask;
extern uint32_t g_vblank_count;   /* PS1 vblanks since start */
void io_init(void);
void io_vblank(void);

/* files.c */
uint8_t *load_file(const char *name, uint32_t *size);
