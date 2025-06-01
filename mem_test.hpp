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
#include <unordered_set>
#include <stdexcept>
#include <mutex>
#include <atomic>

#include "mem_types.hpp"
#include "mem_config.hpp"
#include "mem_context.hpp"
#include "tools.hpp"
#include "mem_count_and_plan.hpp"
#include <sstream>


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
        printf("Loading data...\n");
        uint32_t tot_chunks = 0;
        uint32_t tot_ops = 0;
        uint32_t chunk_id;
        int32_t chunk_size;
        BusDataChunk *chunk_data;
        size_t tot_size = 0;
        // while ((chunk_id = chunks.size()) < MAX_CHUNKS && (chunk_size = load_from_file(chunk_id, &chunk_data)) >=0) {
        while (chunk_id < MAX_CHUNKS && (chunk_size = load_from_file(chunk_id, &chunk_data)) >=0) {
            tot_size += calculate_size2(chunk_data, chunk_size);

            free(chunk_data);
            tot_chunks += chunk_size;
            if (chunk_id % 100 == 0) printf("Loaded chunk %d with size %d\n", chunk_id, chunk_size);
            ++chunk_id;
        }
        printf("TOTAL_SIZE: %ld MB\n", tot_size >> 20);
        printf("chunks: %ld  tot_chunks: %d tot_ops: %d tot_time:%ld (ms)\n", chunks.size(), tot_chunks, tot_ops, (chunks.size() * TIME_US_BY_CHUNK)/1000);
    }

    size_t calculate_size (BusDataChunk *full_chunk_data, uint32_t chunk_size, uint64_t *compact_data = nullptr) {
        BusDataChunk *chunk_data_end = full_chunk_data + chunk_size;
        BusDataChunk *chunk_data = full_chunk_data;
        uint32_t step_size = 0;
        uint64_t selected_main_step  = 0;
        // reservation for worse case
        size_t out_size = 0;
        bool precompiled = false;
        while (chunk_data < chunk_data_end) {
            uint64_t main_step = (chunk_data->data[2] - 1) >> 2;
            uint64_t mem_step_offset = (chunk_data->data[2] - 1) % 4;
            if (step_size == 0) {
                step_size++;
                selected_main_step = main_step;
            } else if (selected_main_step == main_step) {
                step_size++;
            } else {
                out_size += (step_size * 4);
                step_size = 0;
                precompiled = false;
                continue;
            }
            if (mem_step_offset == 3) {
                step_size += 2; // value
            }
            if (mem_step_offset == 2) {
                if (!precompiled) {
                    ++step_size;    // add uint32_t structure to store precompiled reads, precompiled writes
                    precompiled = true;
                }
            }
            ++chunk_data;
        }
        if (step_size > 0) {
            out_size += (step_size * 4);
        }
        return out_size;
    }

    size_t calculate_size2 (BusDataChunk *full_chunk_data, uint32_t chunk_size, uint64_t *compact_data = nullptr) {
        BusDataChunk *chunk_data_end = full_chunk_data + chunk_size;
        BusDataChunk *chunk_data = full_chunk_data;
        size_t out_size = 0;

        std::unordered_set<uint32_t> addresses;
        while (chunk_data < chunk_data_end) {
            uint64_t main_step = (chunk_data->data[2] - 1) >> 2;
            uint64_t mem_step_offset = (chunk_data->data[2] - 1) % 4;
            uint32_t addr = chunk_data->data[1];
            bool is_read = (chunk_data->data[0] == 1);
            auto [iter1, absolute_step] = addresses.insert(addr);
            if (is_read) {
                out_size += absolute_step ? 8 : 4;
            } else {
                uint64_t value = chunk_data->data[6];
                if (value == 0) {
                    out_size += absolute_step ? 12 : 4; // write zero
                } else if (value > 0xFFFFFFFF) {
                    out_size += absolute_step ? 16:12; // write value
                } else {
                    out_size += absolute_step ? 12:8; // write value
                }
            }
            ++chunk_data;
        }
        return out_size;
    }

    // 0:OP|1:ADDR|2:STEP|3:BYTES|MEM_VALUE_0|MEM_VALUE_1|VALUE
    uint32_t prepare_data (BusDataChunk *full_chunk_data, uint32_t chunk_size, uint64_t *compact_data = nullptr) {
        uint32_t counts[MAX_THREADS + 1] = {0};
        BusDataChunk *chunk_data_end = full_chunk_data + chunk_size;
        BusDataChunk *chunk_data = full_chunk_data;
        BusDataChunk *chunk_main_step_data[MAX_MEM_OPS_BY_MAIN_STEP];
        uint32_t chunk_main_step_index = 0;
        uint64_t selected_main_step  = 0;
        // reservation for worse case
        uint64_t *out_data_base = (uint64_t *)malloc(chunk_size * sizeof(uint64_t) * 3);
        uint64_t *out_data = out_data_base;
        while (chunk_data < chunk_data_end) {
            uint64_t main_step = (chunk_data->data[2] - 1) >> 2;
            uint64_t mem_step_offset = (chunk_data->data[2] - 1) % 4;
            if (chunk_main_step_index == 0) {
                chunk_main_step_data[chunk_main_step_index++] = chunk_data;
                selected_main_step = main_step;
            } else if (selected_main_step == main_step) {
                chunk_main_step_data[chunk_main_step_index++] = chunk_data;
            } else {
                out_data = process_main_step_data(selected_main_step, chunk_main_step_data, chunk_main_step_index, out_data);
                chunk_main_step_index = 0;
                continue;
            }
            if (mem_step_offset >= 2) {
                if (mem_step_offset == 2) {
                    throw std::runtime_error("Invalid case2");
                }
                out_data = process_main_step_data(selected_main_step, chunk_main_step_data, chunk_main_step_index, out_data);
                chunk_main_step_index = 0;
            }
            ++chunk_data;
        }
        if (chunk_main_step_index > 0) {
            out_data = process_main_step_data(selected_main_step, chunk_main_step_data, chunk_main_step_index, out_data);
        }
        uint32_t size = (uint8_t *)out_data - (uint8_t *)out_data_base;
        printf("Process size %d\n", size);
        return size;
    }
    #define READ_A_FLAG 0x80000000
    #define READ_B_SHIFT 28
    #define WRITE_C_SHIFT 24
    uint32_t bytes_to_flags(uint32_t bytes) {
        switch (bytes) {
            case 1: return 0b00000100; // 1 byte
            case 2: return 0b00000101; // 2 bytes
            case 4: return 0b00000110; // 4 bytes
            case 8: return 0b00000111; // 8 bytes
            default: {
                std::ostringstream msg;
                msg << "Invalid byte size: 0x" << std::hex << bytes;
                throw std::runtime_error(msg.str());
            }
        }
        // 0b00000000 // none
        // 0b00000001 // precompiled => 8 bytes
    }

    // PRECOMPILED
    uint64_t *process_main_step_data(uint32_t main_step, BusDataChunk **chunk_main_step_data, uint32_t chunk_main_step_index, uint64_t *out_data) {
        // printf("process_main_step_data(%d,,%d,)\n", main_step, chunk_main_step_index);
        uint32_t *step_flags = (uint32_t *)out_data;
        *step_flags = main_step;
        ++out_data;
        for (uint32_t index = 0; index < chunk_main_step_index; ++index) {
            uint8_t mem_step_offset = (chunk_main_step_data[index]->data[2] - 1) % 2;
            switch (mem_step_offset) {
                case 0:
                    // printf("case 0: chunk_main_step_data[%d] = [0x%lX, 0x%lX, 0x%lX, 0x%lX, 0x%lX, 0x%lX, 0x%lX]\n", index,
                    //        chunk_main_step_data[index]->data[0],
                    //        chunk_main_step_data[index]->data[1],
                    //        chunk_main_step_data[index]->data[2],
                    //        chunk_main_step_data[index]->data[3],
                    //        chunk_main_step_data[index]->data[4],
                    //        chunk_main_step_data[index]->data[5],
                    //        chunk_main_step_data[index]->data[6]);
                    *step_flags |= READ_A_FLAG;
                    *out_data = chunk_main_step_data[index]->data[1];
                    ++out_data;
                    break;
                case 1:
                    // printf("case 1: chunk_main_step_data[%d] = [0x%lX, 0x%lX, 0x%lX, 0x%lX, 0x%lX, 0x%lX, 0x%lX]\n", index,
                    //        chunk_main_step_data[index]->data[0],
                    //        chunk_main_step_data[index]->data[1],
                    //        chunk_main_step_data[index]->data[2],
                    //        chunk_main_step_data[index]->data[3],
                    //        chunk_main_step_data[index]->data[4],
                    //        chunk_main_step_data[index]->data[5],
                    //        chunk_main_step_data[index]->data[6]);
                    *step_flags |= (bytes_to_flags(chunk_main_step_data[index]->data[3]) << READ_B_SHIFT);
                    *out_data = chunk_main_step_data[index]->data[1];
                    ++out_data;
                    break;
                case 2:
                    throw std::runtime_error("Invalid case");
                    break;
                case 3:
                    // printf("case 3: chunk_main_step_data[%d] = [0x%lX, 0x%lX, 0x%lX, 0x%lX, 0x%lX, 0x%lX, 0x%lX]\n", index,
                    //        chunk_main_step_data[index]->data[0],
                    //        chunk_main_step_data[index]->data[1],
                    //        chunk_main_step_data[index]->data[2],
                    //        chunk_main_step_data[index]->data[3],
                    //        chunk_main_step_data[index]->data[4],
                    //        chunk_main_step_data[index]->data[5],
                    //        chunk_main_step_data[index]->data[6]);

                    *step_flags |= (bytes_to_flags(chunk_main_step_data[index]->data[3]) << READ_B_SHIFT);
                    *out_data = chunk_main_step_data[index]->data[1];
                    ++out_data;
                    break;
                default:
                    throw std::runtime_error("Invalid memory step offset");
            }

        }
        // 1 -> 0
        // 2 -> 0, 1
        // 3 -> 0, 1, 2
        ++out_data;
        return out_data;
    }
/*
            if (chunk_main_step_index < 2) {
                // Not enough data to process
                return;
            }
            if (chunk_main_step_index == 2) {
                // Only one step data, process it directly
                MemCountersBusData *chunk_data = (MemCountersBusData *)malloc(sizeof(MemCountersBusData));
                if (chunk_data == nullptr) {
                    throw std::runtime_error("Memory allocation failed");
                }
                *chunk_data = *(MemCountersBusData *)chunk_main_step_data[1];

 chunk_main_step_data[2] = chunk_data;
                // Process the main step data
                const uint32_t bytes = get_bytes_from_flags(chunk_main_step_data[2]->data[3]);
                const uint32_t addr = chunk_main_step_data[0]->data[1];
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
                chunk_main_step_index = -1; // Reset for next set of data
            }


            const uint32_t bytes = get_bytes_from_flags(chunk_data->data[3]);
            const uint32_t addr = chunk_data->data[1];
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
            const uint32_t bytes = get_bytes_from_flags(chunk_data->data[3]);
            const uint32_t addr = chunk_data->data[1];
            const uint32_t addr_w = addr >> 3;
            if (bytes == 8 && (addr & 0x07) == 0) {
                uint32_t index = addr_w & ADDR_W_MASK;
                data[index][indexes[index]] = *chunk_data;
                indexes[index] += 1;
            } else {
                uint32_t index = addr_w & ADDR_W_MASK;
                data[index][indexes[index]] = *chunk_data;
*/
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