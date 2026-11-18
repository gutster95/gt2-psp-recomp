/* SPU + CD-XA audio (spu.c) */
#pragma once
#include <stdint.h>

void spu_init(void);
uint32_t spu_read16(uint32_t off);              /* off: address - 0x1F800000 (0x1C00-0x1FFF) */
void spu_write16(uint32_t off, uint32_t v);
void spu_dma(uint32_t madr, uint32_t words, int to_spu);
void spu_xa_sector(const uint8_t *raw_sector);  /* decoded XA-ADPCM audio sector (2352 bytes) */
