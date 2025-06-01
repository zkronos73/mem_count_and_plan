#ifndef __MEM_CONTEXT_HPP__
#define __MEM_CONTEXT_HPP__

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
#include "mem_locators.hpp"

class MemContext {
public:
    #ifdef SPLITTED_CHUNKS
    MemChunk chunks[MAX_THREADS+1][MAX_CHUNKS];
    #else
    MemChunk chunks[MAX_CHUNKS];
    #endif
    MemLocators locators;
    std::atomic<uint32_t> chunks_count;
    std::atomic<bool> chunks_completed;
    void clear () {
        chunks_count.store(0, std::memory_order_release);
        chunks_completed.store(false, std::memory_order_release);
    }
    #ifdef SPLITTED_CHUNKS
    const MemChunk *get_chunk(uint32_t thread_id, uint32_t chunk_id) {
        while (chunk_id >= chunks_count.load(std::memory_order_acquire)) {
            if (chunks_completed.load(std::memory_order_acquire)) {
                return nullptr;
            }
            usleep(1);
        }
        const MemChunk *result = &chunks[thread_id][chunk_id];
        // if (result == nullptr) {
        //     printf("get_chunk: thread_id=%d chunk_id=%d NULL", thread_id, chunk_id);
        // } else {
        //     printf("get_chunk: thread_id=%d chunk_id=%d data=%p count=%d\n", thread_id, chunk_id, result->data, result->count);
        // }
        return result;
    }
    #else
    const MemChunk *get_chunk(uint32_t chunk_id) {
        while (chunk_id >= chunks_count.load(std::memory_order_acquire)) {
            if (chunks_completed.load(std::memory_order_acquire)) {
                return nullptr;
            }
            usleep(1);
        }
        return &chunks[chunk_id];
    }
    #endif

    MemContext() : chunks_count(0), chunks_completed(false) {
    }
    #ifdef SPLITTED_CHUNKS
    void add_chunk(MemCountersBusData **data, uint32_t *count) {
        uint32_t chunk_id = chunks_count.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i <= MAX_THREADS; ++i) {
            chunks[i][chunk_id].data = data[i];
            chunks[i][chunk_id].count = count[i];
        }
        chunks_count.store(chunk_id + 1, std::memory_order_release);
    }
    #else
    void add_chunk(MemCountersBusData *data, uint32_t count) {
        uint32_t chunk_id = chunks_count.load(std::memory_order_relaxed);
        chunks[chunk_id].data = data;
        chunks[chunk_id].count = count;
        chunks_count.store(chunk_id + 1, std::memory_order_release);
    }
    #endif
    void set_completed() {
        chunks_completed.store(true, std::memory_order_release);
    }
    uint32_t size() {
        return chunks_count.load(std::memory_order_acquire);
    }
};

#endif