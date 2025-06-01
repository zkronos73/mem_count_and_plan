#ifndef __MEM_TEST_HPP__
#define __MEM_TEST_HPP__

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <vector>
#include <thread>
#include <iostream>
#include <string.h>
#include <sys/time.h>
#include <cstdint>
#include <vector>
#include <map>
#include <unordered_map>
#include <stdexcept>
#include <mutex>
#include <atomic>

#include "mem_types.hpp"
#include "mem_config.hpp"
#include "mem_context.hpp"
#include "tools.hpp"
#include "mem_count_and_plan.hpp"


#ifdef SPLITTED_CHUNKS

class MemTestChunk {
public:
    MemCountersBusData *chunk_data[MAX_THREADS + 1];
    uint32_t chunk_size[MAX_THREADS + 1];
    MemTestChunk(MemCountersBusData **data, uint32_t *size) {
        for (uint32_t i = 0; i <= MAX_THREADS; ++i) {
            chunk_data[i] = data[i];
            chunk_size[i] = size[i];
        }
    }
    ~MemTestChunk() {
//        free(chunk_data);
    }
};

class MemTest {
private:
    std::vector<MemTestChunk> chunks;
public:
    MemTest() {
        chunks.reserve(4096);
    }
    ~MemTest() {
        for (auto& chunk : chunks) {
            for (uint32_t index = 0; index < MAX_THREADS + 1; ++index) {
                free(chunk.chunk_data[index]);
            }
        }
    }
    void load() {
        printf("Loading compact data...\n");
        uint32_t tot_chunks = 0;
        uint32_t chunk_id;
        int32_t chunk_size;
        MemCountersBusData *chunk_data;
        while ((chunk_id = chunks.size()) < MAX_CHUNKS && (chunk_size = load_from_compact_file(chunk_id, &chunk_data)) >=0) {
            divide_and_push(chunk_data, chunk_size);
            free(chunk_data);
            tot_chunks += chunk_size;
            if (chunk_id % 100 == 0) printf("Loaded chunk %d with size %d\n", chunk_id, chunk_size);
        }
        printf("chunks: %ld  tot_chunks: %d tot_time:%ld (ms)\n", chunks.size(), tot_chunks, (chunks.size() * TIME_US_BY_CHUNK)/1000);
    }

    void divide_and_push (MemCountersBusData *full_chunk_data, uint32_t chunk_size) {
        const uint32_t chunk_id = 0; // Assuming a single chunk for simplicity
        uint32_t counts[MAX_THREADS + 1] = {0};
        MemCountersBusData *chunk_data_end = full_chunk_data + chunk_size;
        MemCountersBusData *chunk_data = full_chunk_data;
        while (chunk_data < chunk_data_end) {
            const uint32_t bytes = get_bytes_from_flags(chunk_data->flags);
            const uint32_t addr = chunk_data->addr;
            const uint32_t addr_w = addr >> 3;
            if (bytes == 8 && (addr & 0x07) == 0) {
                counts[addr_w & ADDR_W_MASK] += 1;
            } else {
                counts[addr_w & ADDR_W_MASK] += 1;
                counts[MAX_THREADS] += 1;
                if ((bytes + (addr & 0x07)) > 8) {
                    counts[(addr_w + 1) & ADDR_W_MASK] += 1;
                }
            }
            ++chunk_data;
        }
        MemCountersBusData *data[MAX_THREADS + 1];
        for (uint32_t i = 0; i <= MAX_THREADS; ++i) {
            if (counts[i] > 0) {
                data[i] = (MemCountersBusData *)malloc(counts[i] * sizeof(MemCountersBusData));
                if (data[i] == nullptr) {
                    throw std::runtime_error("Memory allocation failed");
                }
            } else {
                data[i] = nullptr;
            }
        }
        uint32_t indexes[MAX_THREADS + 1] = {0};
        chunk_data = full_chunk_data;
        while (chunk_data < chunk_data_end) {
            const uint32_t bytes = get_bytes_from_flags(chunk_data->flags);
            const uint32_t addr = chunk_data->addr;
            const uint32_t addr_w = addr >> 3;
            if (bytes == 8 && (addr & 0x07) == 0) {
                uint32_t index = addr_w & ADDR_W_MASK;
                data[index][indexes[index]] = *chunk_data;
                indexes[index] += 1;
            } else {
                uint32_t index = addr_w & ADDR_W_MASK;
                data[index][indexes[index]] = *chunk_data;
                indexes[index] += 1;

                data[MAX_THREADS][indexes[MAX_THREADS]] = *chunk_data;
                indexes[MAX_THREADS] += 1;

                if ((bytes + (addr & 0x07)) > 8) {
                    uint32_t index = (addr_w + 1) & ADDR_W_MASK;
                    data[index][indexes[index]] = *chunk_data;
                    indexes[index] += 1;
                }
            }
            ++chunk_data;
        }
        chunks.emplace_back(data, counts);
    }

    void execute(void) {
        printf("Starting...\n");
        auto cp = create_mem_count_and_plan();
        printf("Executing...\n");
        execute_mem_count_and_plan(cp);
        uint64_t init = get_usec();
        uint32_t chunk_id = 0;
        for (auto& chunk : chunks) {
            uint64_t chunk_ready = init + (uint64_t)(chunk_id+1) * TIME_US_BY_CHUNK;
            uint64_t current = get_usec();
            if (current < chunk_ready) {
                usleep(chunk_ready - current);
            }
            add_chunk_mem_count_and_plan(cp, chunk.chunk_data, chunk.chunk_size);
            ++chunk_id;
        }
        set_completed_mem_count_and_plan(cp);
        wait_mem_count_and_plan(cp);
        stats_mem_count_and_plan(cp);
    }
};

#else

class MemTestChunk {
public:
    MemCountersBusData *chunk_data;
    uint32_t chunk_size;
    MemTestChunk(MemCountersBusData *data, uint32_t size) : chunk_data(data), chunk_size(size) {}
    ~MemTestChunk() {
//        free(chunk_data);
    }
};

class MemTest {
private:
    std::vector<MemTestChunk> chunks;
public:
    MemTest() {
        chunks.reserve(4096);
    }
    ~MemTest() {
        for (auto& chunk : chunks) {
            free(chunk.chunk_data);
        }
    }
    void load() {
        printf("Loading compact data...\n");
        uint32_t tot_chunks = 0;
        uint32_t tot_ops = 0;
        uint32_t chunk_id;
        int32_t chunk_size;
        MemCountersBusData *chunk_data;
        while ((chunk_id = chunks.size()) < MAX_CHUNKS && (chunk_size = load_from_compact_file(chunk_id, &chunk_data)) >=0) {
            chunks.emplace_back(chunk_data, chunk_size);
            tot_ops += count_operations(chunk_data, chunk_size);
            tot_chunks += chunk_size;
            if (chunk_id % 100 == 0) printf("Loaded chunk %d with size %d\n", chunk_id, chunk_size);
        }
        printf("chunks: %ld  tot_chunks: %d tot_ops: %d tot_time:%ld (ms)\n", chunks.size(), tot_chunks, tot_ops, (chunks.size() * TIME_US_BY_CHUNK)/1000);
    }

    void execute(void) {
        printf("Starting...\n");
        auto cp = create_mem_count_and_plan();
        printf("Executing...\n");
        execute_mem_count_and_plan(cp);
        uint64_t init = get_usec();
        uint32_t chunk_id = 0;
        for (auto& chunk : chunks) {
            uint64_t chunk_ready = init + (uint64_t)(chunk_id+1) * TIME_US_BY_CHUNK;
            uint64_t current = get_usec();
            if (current < chunk_ready) {
                usleep(chunk_ready - current);
            }
            MemCountersBusData *data = chunk.chunk_data;
            uint32_t chunk_size = chunk.chunk_size;
            add_chunk_mem_count_and_plan(cp, data, chunk_size);
            ++chunk_id;
        }
        set_completed_mem_count_and_plan(cp);
        wait_mem_count_and_plan(cp);
        stats_mem_count_and_plan(cp);
    }
};

#endif

#endif