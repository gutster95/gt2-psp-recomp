/* Memory card (card.c) */
#pragma once
#include <stdint.h>

void card_init(void);
void card_tick(void);                     /* every vblank: background saving */
uint32_t card_info(uint32_t port);
uint32_t card_read(uint32_t port, uint32_t sector, uint32_t dst);
uint32_t card_write(uint32_t port, uint32_t sector, uint32_t src);

/* bios.c: deliver an event (class, spec) to its callbacks shortly, in interrupt context */
void bios_event_later(uint32_t cls, uint32_t spec);
