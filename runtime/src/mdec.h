#ifndef GT2_MDEC_H
#define GT2_MDEC_H
#include <stdint.h>

void mdec_reset(void);
void mdec_write(uint32_t reg, uint32_t v);      /* reg 0 = 1F801820 (command/data), 4 = 1F801824 (control) */
uint32_t mdec_read(uint32_t reg);               /* reg 0 = data out, 4 = status */
void mdec_dma_in(uint32_t madr, uint32_t words);
int mdec_dma_out(uint32_t madr, uint32_t words);   /* 0: deferred until the input is complete */
void io_dma_done(int ch);                       /* io.c: finish a deferred DMA */

#endif
