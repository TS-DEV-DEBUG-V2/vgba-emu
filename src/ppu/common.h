#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#ifndef PPU_COMMON_H
#define PPU_COMMON_H

#define SCREEN_WIDTH   240
#define SCREEN_HEIGHT  160
#define VDRAW_LINES    160
#define TOTAL_LINES    228

#define BIT(v, b)      (((v) >> (b)) & 1)
#define BITS(v, h, l)  (((v) >> (l)) & ((1 << ((h)-(l)+1)) - 1))

#endif
