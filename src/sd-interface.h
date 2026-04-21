#ifndef __SD_INTERFACE_H
#define __SD_INTERFACE_H

#include <stdint.h>

// note - "block"/"sector" are used interchangeably in this code

#define BLOCK_SIZE 512          // block/sector size
#define BLOCK_SIZE_SHIFT 9      // 1 << BLOCK_SIZE_SHIFT = BLOCK_SIZE

// bitmask to select BLOCK_SIZE_SHIFT lower bits
#define BLOCK_SIZE_MASK 0x1ff

int read_block(uint32_t addr, void *dst);

#endif