#pragma once
#include <stdint.h>

void cdrom_init(void);
void cdrom_tick(void);
uint32_t cdrom_read(uint32_t reg);
void cdrom_write(uint32_t reg, uint32_t v);
void cdrom_dma(uint32_t madr, uint32_t words);
int cdrom_read_data(uint32_t lba, uint32_t count, uint8_t *dst);   /* 2048-byte data sectors from the pack */
