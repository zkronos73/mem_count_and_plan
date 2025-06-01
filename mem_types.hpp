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

#define MEM_BUS_DATA_SIZE 7 // Replace with your actual size

typedef struct {
    uint64_t data[MEM_BUS_DATA_SIZE];
} BusDataChunk;

// NON ALIGNED
//
// addr_w = addr >> 3
// a_write = aligned_write
//
// SINGLE READ [MEM_THREAD_i] => addr_w read; unaligned.push(value)
//
// DOUBLE READ [MEM_THREAD_i] => addr_w read; unaligned_push(value_0)
//             [MEM_THREAD_j] => addr_w + 8 read; unaligned_push(value_1)
//
// SINGLE WRITE [MEM_THREAD_i] => addr_w data = a_write; unaligned.push(value)
//
// DOUBLE WRITE [MEM_THREAD_i] => addr_w data = a_write_0; unaligned_push(value_0)
//              [MEM_THREAD_j] => addr_w + 8 data = a_write_1; unaligned_push(value_1)
//
// unaligned (by thread)

// RD_REL             |3: type(RD_REL)                 |29: relative_step   |1 x 32 bits
// RD_ABS             |3: type(RD_ABS)                 |61: absolute_step   |2 x 32 bits
// WR_REL_ZERO        |3: type(WR_REL_ZERO)            |29: relative_step   |1 x 32 bits
// WR_REL_SHORT_DATA  |3: type(WR_REL_SHORT_DATA)      |29: relative_step   |2 x 32 bits
// WR_REL_LONG_DATA   |3: type(WR_REL_LONG_DATA)       |29: relative_step   |3 x 32 bits
// WR_ABS_SHORT_DATA  |3: type(WR_ABS_SHORT_DATA)      |29: relative_step   |3 x 32 bits
// WR_ABS_LONG_DATA   |3: type(WR_ABS_LONG_DATA)       |29: relative_step   |4 x 32 bits

// REL_STEP (25 bits) + READ_A (1 bit), READ_B (3 bits), WRITE_C (3 bits) = 32 bits
// [ADDR_A (32 bits)] + [ADDR_B (32 bits)] + [ADDR_C (32 bits) + VALUE (64 bits)] <= MAX (32 * 4 + 64) = 192 bits => 64 bits x 3

// ONE READ => 32 + 32 = 64 bits = 8 bytes
// TWO READS => 32 + 32 * 2 = 96 bits = 12 bytes
// ONE WRITE => 32 + 32 + 64 = 128 bits = 16 bytes
// ONE PRECOMPILED (WRITE) => 32 + 32 (count_r + count_w << 16) + 32 * count_r + (32 + 64) * count_w = 8 + 4 * count_r + 12 * count_w
// ONE READ + ONE WRITE => 32 + 32 + 32 + 64 = 160 bits = 20 bytes
// ONE READ + ONE PRECOMPILED (WRITE) => 32 + 32 + 32 (count_r + count_w << 16) + 32 * count_r + (32 + 64) * count_w = 12 + 4 * count_r + 12 * count_w
// TWO READS + ONE WRITE 64 BITS => 32 + 32 * 3 + 64 = 24 bytes
// TWO READS + ONE PRECOMPILED (WRITE) => 32 + 2 * 32 + 32 (count_r + count_w << 16) + 32 * count_r + (32 + 64) * count_w = 16 + 4 * count_r + 12 * count_w

// PRECOMPILED ??????

// NON POSSIBLE, DIRTY WRITE
// --> ONE WRITE ZERO => 32 + 32 = 64 bits = 8 bytes
// --> ONE WRITE 32 BITS => 32 + 32 + 32 = 96 bits = 12 bytes
// --> TWO READS + ONE WRITE ZERO => 32 + 32 * 3 = 16 bytes
// --> TWO READS + ONE WRITE 32 BITS => 32 + 32 * 3 + 32 = 20 bytes

// PROBLEM: INPUT => @9000_0000 PROBLEM (add on sequencial INPUT memory added each time that write 0x9000 like precompiled)
//          INPUT => 9000_0000 READ DIRECTLY


struct MemTrace {
    MemCountersBusData *chunk_data[MAX_CHUNKS];
    uint32_t chunk_size[MAX_CHUNKS];
    uint32_t chunks = 0;
};


#endif