#pragma once
#include <stdint.h>

void gpu_init(void);
void gpu_gp0(uint32_t v);
void gpu_gp1(uint32_t v);
uint32_t gpu_read(void);
uint32_t gpu_status(void);
void gpu_present(void);
extern uint16_t *g_vram;   /* 1024 x 512, 16 bpp */
