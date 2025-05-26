#ifndef __MEM_TYPES_HPP__
#define __MEM_TYPES_HPP__

#include <stdint.h>
#include "mem_config.hpp"

struct MemCountersBusData {
    uint32_t addr;
    uint32_t flags;
};

struct MemChunk {
    MemCountersBusData *data;
    uint32_t count;
};

struct MemCountTrace {
    MemCountersBusData *chunk_data[MAX_CHUNKS];
    uint32_t chunk_size[MAX_CHUNKS];
    uint32_t chunks = 0;
};

// RD_REL             |3: type(RD_REL)                 |29: relative_step   |1 x 32 bits
// RD_ABS             |3: type(RD_ABS)                 |61: absolute_step   |2 x 32 bits
// WR_REL_ZERO        |3: type(WR_REL_ZERO)            |29: relative_step   |1 x 32 bits
// WR_REL_SHORT_DATA  |3: type(WR_REL_SHORT_DATA)      |29: relative_step   |2 x 32 bits
// WR_REL_LONG_DATA   |3: type(WR_REL_LONG_DATA)       |29: relative_step   |3 x 32 bits
// WR_ABS_SHORT_DATA  |3: type(WR_ABS_SHORT_DATA)      |29: relative_step   |3 x 32 bits
// WR_ABS_LONG_DATA   |3: type(WR_ABS_LONG_DATA)       |29: relative_step   |4 x 32 bits

struct MemTrace {
    MemCountersBusData *chunk_data[MAX_CHUNKS];
    uint32_t chunk_size[MAX_CHUNKS];
    uint32_t chunks = 0;
};


#endif