/* GPU emulation on the Media Engine (settings.txt me_gpu=1), see me.c. */
#pragma once
#include <stdint.h>

extern int g_me_gpu;                 /* settings: run the GPU path on the ME (if the ME could be started) */
extern int g_me_active;              /* the ME is running the GPU path */

/* code running on the ME has its stack in the ME's local eDRAM (kseg0), the main CPU's threads in user RAM */
static inline int me_on_me(void)
{
    uint32_t sp;
    __asm__ volatile("move %0, $sp" : "=r"(sp));
    return sp >= 0x80000000u;
}

/* uncached view of a variable (data the other CPU writes) */
#define ME_UC(var) (*(volatile __typeof__(var) *)((uintptr_t)&(var) | 0x40000000u))

/* ---- main CPU ---- */
void me_gpu_start(void);             /* after gpu_init/ge_init: start the ME (no-op unless g_me_gpu) */
void me_post_gp0(uint32_t v);
void me_post_gp1(uint32_t v);
void me_post_walk(uint32_t madr);    /* DMA2 linked list: io_dma_done(2) when the ME has walked it */
void me_post_block(uint32_t madr, uint32_t words, int to_gpu);   /* DMA2 block mode, either direction */
void me_post_vblank(void);
uint32_t me_gpu_read(void);          /* GPUREAD port (waits for the ME) */
uint32_t me_gpu_status(void);        /* GPUSTAT as published by the ME */
void me_service(void);               /* rt_poll: DMA completion, ME log lines */
void me_join(void);                  /* wait until the ME has processed everything posted */

/* ---- ME side (called by ge.c / gpu.c / log.c / core.c when running on the ME) ---- */
void me_log(const char *fmt, const uint32_t *args, int n);
uint64_t me_now_us(void);
uint32_t me_vcount(void);            /* display vcount, published by the main CPU's vblank thread */
/* hand a finished GE list to the main CPU: enqueue [start, end); show >= 0: display that buffer when the GE is done
 * (fps digits drawn first); wait: return only when the GE has finished it. Returns the request sequence number. */
uint32_t me_ge_submit(const void *start, const void *end, int show, int fps, int wait);
int me_ge_done(uint32_t seq);        /* the GE has finished that request */
uint32_t me_swap_vcount(void);       /* vcount when the last display swap was made */
void me_dcache_wb_range(const void *p, uint32_t bytes);
void me_dcache_wbinv_all(void);
