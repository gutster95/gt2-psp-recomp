#pragma once
#include <stdint.h>

void sio_init(void);
uint32_t sio_read(uint32_t off, int size);    /* off relative to 0x1F801040 */
void sio_write(uint32_t off, uint32_t v, int size);
