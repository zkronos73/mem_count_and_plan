#ifndef __MEM_ALIGN_COUNTER_HPP__
#define __MEM_ALIGN_COUNTER_HPP__

#include "mem_config.hpp"
#include "mem_types.hpp"
#include "tools.hpp"
#include <vector>


class MemAlignCheckPoint {
public:
    std::vector<uint32_t> chunks;
    uint32_t skip;
    uint32_t count;
    MemAlignCheckPoint() : skip(0), count(0) {
    }
};

class MemAlignCounter {
private:
    const MemCountTrace *mcp;
    MemAlignCheckPoint *current_cp;
    std::vector<MemAlignCheckPoint *> checkpoints;
    uint32_t count;
    uint32_t available_rows;
    uint32_t current_chunk_count;
    uint32_t skip;
    uint32_t last_chunk_id;
    uint32_t rows;
    uint64_t init;
    uint32_t ellapsed_ms;
public:
    uint32_t last_addr[MAX_PAGES];
    MemAlignCounter(uint32_t rows, const MemCountTrace *mcp, uint64_t init) :mcp(mcp), rows(rows), init(init) {
        count = 0;
        available_rows = rows;
        skip = 0;
        last_chunk_id = 0xFFFFFFFF;
        current_cp = new MemAlignCheckPoint();
    }
    ~MemAlignCounter() {
    }
    void close_checkpoint(bool last = false) {
        current_cp->count = current_chunk_count + count;
        current_cp->skip = skip;
        if (current_cp->count > 0) {
            checkpoints.push_back(current_cp);
        }
        if (last) {
            return;
        }

        current_cp = new MemAlignCheckPoint();

        if (current_chunk_count > 0) {
            current_cp->skip = current_chunk_count;
            current_cp->chunks.push_back(last_chunk_id);
        }

        last_chunk_id = 0xFFFFFFFF;
        count = 0;
        current_chunk_count = 0;
        available_rows = rows;
    }
    void close_chunk() {
        count += current_chunk_count;
        current_chunk_count = 0;
    }
    void execute();
    void add_mem_align_op(uint32_t chunk_id, uint32_t ops) {
        if (available_rows < ops) {
            close_checkpoint();
        }
        if (last_chunk_id != chunk_id) {
            current_cp->chunks.push_back(chunk_id);
            last_chunk_id = chunk_id;
        }
        available_rows -= ops;
        ++current_chunk_count;
    }
    uint32_t get_instances_count() {
        return checkpoints.size();
    }
    uint32_t get_ellapsed_ms() {
        return ellapsed_ms;
    }
};

void MemAlignCounter::execute() {
    const int chunks = mcp->chunks;
    for (int j = 0; j < chunks; ++j) {
        const uint32_t chunk_size = mcp->chunk_size[j];
        const MemCountersBusData *chunk_data = mcp->chunk_data[j];
        for (uint32_t i = 0; i < chunk_size; i++) {
            const uint8_t bytes = chunk_data[i].flags & 0xFF;
            const uint32_t addr = chunk_data[i].addr;
            if (bytes != 8 || (addr & 0x07) != 0) {
                uint32_t ops = ((bytes + (addr & 0x07)) > 8) ? 2:1 * (1 + (chunk_data[i].flags >> 16)) + 1;
                add_mem_align_op(j, ops);
            }
        }
        close_chunk();
        uint64_t next_chunk = init + (uint64_t)(j+1) * TIME_US_BY_CHUNK;
        uint64_t current = get_usec();
        if (current < next_chunk) {
            usleep(next_chunk - current);
        }
    }
    close_checkpoint(true);
    ellapsed_ms = ((get_usec() - init) / 1000);
}

#endif // __MEM_ALIGN_COUNTER_HPP__