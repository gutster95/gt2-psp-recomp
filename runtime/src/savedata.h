/* PSP save data utility storage of the memory card image (savedata.c) */
#pragma once
#include <stdint.h>

#define SAVEDATA_CARD_SIZE (128 * 1024)

int savedata_load(uint8_t *card);         /* blocking; 1 if a save was loaded */
void savedata_written(void);              /* the game wrote a sector */
void savedata_tick(const uint8_t *card);  /* every vblank: background saving */
