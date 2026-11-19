#pragma once
#include <stdint.h>

void gpu_init(void);
uint32_t gpu_draw_mode(void);
void gpu_set_draw_mode(uint32_t m);
void ge_bench_file(void);   /* io.c: settings.txt gebenchfile=1 */
void gpu_gp0(uint32_t v);
void gpu_packet(const uint32_t *w, uint32_t n);   /* n command words of a DMA linked-list packet */
void gpu_gp1(uint32_t v);
uint32_t gpu_read(void);
uint32_t gpu_status(void);
void gpu_present(void);
extern uint16_t *g_vram;   /* 1024 x 512, 16 bpp */
