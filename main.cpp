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
#include <cstdint>  // Per a uint32_t
#include <vector>
#include <map>
#include <unordered_map>
#include <stdexcept>
#include <mutex>
#include <atomic>

#include "mem_types.hpp"
#include "mem_config.hpp"
#include "tools.hpp"
#include "mem_counter.hpp"
#include "mem_align_counter.hpp"
#include "mem_planner.hpp"
#include "mem_locator.hpp"

// TODO: additional thread to sort memories address vs pages
// TODO: shared memory slots to balance in a worst scenario
// TODO: incremental memory slots on worst scenario (consolidate full memory slots? to avoid increase).


class LockFreeRingBuffer {
    std::vector<uint32_t> buffer;
    std::atomic<size_t> read_pos{0};
    std::atomic<size_t> write_pos{0};
    size_t capacity;

public:
    LockFreeRingBuffer(size_t size) : buffer(size), capacity(size) {}

    bool try_push(uint32_t value) {
        size_t current_write = write_pos.load(std::memory_order_relaxed);
        size_t next_write = (current_write + 1) % capacity;

        if(next_write == read_pos.load(std::memory_order_acquire)) {
            return false; // full buffer
        }

        buffer[current_write] = value;
        write_pos.store(next_write, std::memory_order_release);
        return true;
    }

    bool try_pop(uint32_t& value) {
        size_t current_read = read_pos.load(std::memory_order_relaxed);

        if(current_read == write_pos.load(std::memory_order_acquire)) {
            return false; // empty buffer
        }

        value = buffer[current_read];
        read_pos.store((current_read + 1) % capacity, std::memory_order_release);
        return true;
    }
};

struct MemAlignCount {
    uint32_t chunk_id;
    uint32_t count[3];
    MemAlignCount(uint32_t chunk_id, uint32_t count[3]) : chunk_id(chunk_id), count{count[0], count[1], count[2]} {}
};

typedef struct {
    int thread_index;
    const MemCountAndPlan *mcp;
    int count;
} MemCountAndPlanThread;

int main() {
    printf("Starting...\n");

    MemCountAndPlan mcp;
    int chunks = 0;
    int tot_chunks = 0;
    uint32_t tot_ops = 0;
    printf("Loading compact data...\n");
    int32_t items_read;
    while (chunks < MAX_CHUNKS && (items_read = load_from_compact_file(chunks, &(mcp.chunk_data[chunks]))) >=0) {
        mcp.chunk_size[chunks] = items_read;
        tot_ops += count_operations(mcp.chunk_data[chunks], mcp.chunk_size[chunks]);
        chunks++;
        tot_chunks += mcp.chunk_size[chunks - 1];
        if (chunks % 100 == 0) printf("Loaded chunk %d with size %d\n", chunks, mcp.chunk_size[chunks - 1]);
    }
    printf("chunks: %d  tot_chunks: %d tot_ops: %d tot_time:%d (ms)\n", chunks, tot_chunks, tot_ops, (chunks * TIME_US_BY_CHUNK)/1000);
    mcp.chunks = chunks;

    printf("Initialization...\n");
    auto start = std::chrono::high_resolution_clock::now();
    std::vector<std::thread> threads;
    std::vector<MemCounter *> workers;

    // Crear 16 threads
    // SortedMemAddressCollector collector;
    uint64_t init = get_usec();
    for (int i = 0; i < MAX_THREADS; ++i) {
        MemCounter *th = new MemCounter(i, &mcp, mcp.chunk_size[i], init);
        workers.push_back(th);
    }
    auto mem_align_counter = new MemAlignCounter(MEM_ALIGN_ROWS, &mcp, init);
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end-start);

    std::cout << "Duration initialization " << duration.count() << " ms" << std::endl;

    start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < MAX_THREADS; ++i) {
        threads.emplace_back([workers, i](){ workers[i]->execute();});
    }
    threads.emplace_back([mem_align_counter](){ mem_align_counter->execute();});

    // collector.wait_for_producers();

    // Esperar a que tots els threads acabin
    for (auto& t : threads) {
        t.join();
    }
    // const std::vector<uint32_t> *sorted = collector.get_sorted();
    end = std::chrono::high_resolution_clock::now();
    duration = std::chrono::duration_cast<std::chrono::milliseconds>(end-start);
    std::cout << "Duration count & sort " << duration.count() << " ms" << std::endl;
    end = std::chrono::high_resolution_clock::now();
    duration = std::chrono::duration_cast<std::chrono::milliseconds>(end-start);
    std::cout << "Duration count & sort (with sorted) " << duration.count() << " ms" << std::endl;

    std::cout << "MemAlign " << mem_align_counter->get_instances_count() << " instances, on " << mem_align_counter->get_ellapsed_ms() << " ms" << std::endl;
    // int tot_count = 0;
    // int min_count = 0xFFFF;
    // int max_count = 0;
    // int extra_unaligned = 0;
    // for (int i = 0; i < MAX_THREADS; ++i) {
    //     int count = workers[i]->get_count();
    //     min_count = std::min(min_count, count);
    //     max_count = std::max(max_count, count);
    //     tot_count += count;
    //     extra_unaligned += workers[i]->get_extra_unaligned();
    // }

    // printf("Counts: min %d max %d tot %d extra %d T %d\n", min_count, max_count, tot_count, extra_unaligned, tot_chunks + extra_unaligned);
    uint32_t tot_used_slots = 0;
    for (int i = 0; i < MAX_THREADS; ++i) {
        uint32_t used_slots = workers[i]->get_used_slots();
        tot_used_slots += used_slots;
        printf("Thread %d: used slots %d/%d (%04.02f%%) T:%d ms S:%d ms Q:%d\n", i, used_slots, ADDR_SLOTS, ((double)used_slots*100.0)/(double)(ADDR_SLOTS), workers[i]->get_ellapsed_ms(), workers[i]->get_tot_usleep()/1000, workers[i]->get_queue_full_times()/1000);
    }
    printf("\n> threads: %d\n", MAX_THREADS);
    printf("> address table: %ld MB\n", (ADDR_TABLE_SIZE * ADDR_TABLE_ELEMENT_SIZE * MAX_THREADS)>>20);
    printf("> memory slots: %ld MB (used: %ld MB)\n", (ADDR_SLOTS_SIZE * sizeof(uint32_t) * MAX_THREADS)>>20, (tot_used_slots * ADDR_SLOT_SIZE * sizeof(uint32_t))>> 20);
    printf("> page table: %ld MB\n\n", (ADDR_PAGE_SIZE * sizeof(uint32_t))>> 20);

/*
    std::vector<uint32_t> addresses;

    start = std::chrono::high_resolution_clock::now();
    // auto end2 = std::chrono::high_resolution_clock::now();
    for (int page = 0; page < MAX_PAGES; ++page) {
        uint32_t page_offset = page * ADDR_PAGE_SIZE;
        uint32_t offset = workers[0]->first_offset[page];
        uint32_t last_offset = workers[0]->last_offset[page];
        for (int i = 1; i < MAX_THREADS; ++i) {
            offset = std::min<uint32_t> (workers[i]->first_offset[page], offset);
            last_offset = std::max<uint32_t> (workers[i]->last_offset[page], last_offset);
        }
        if (offset == 0xFFFFFFFF) {
            continue;
        }
        uint32_t addr = MemCounter::offset_to_addr(offset);
        // uint32_t addr = MemCounter::page_to_addr(page);
        // uint32_t max_offset = page_offset + ADDR_PAGE_SIZE;
        for (; offset <= last_offset; ++offset) {
            for (uint32_t i = 0; i < MAX_THREADS; ++i) {
                uint32_t pos = workers[i]->get_addr_table(offset);
                if (pos != 0) {
                    addresses.push_back(addr);
                    addresses.push_back(pos);
                }
                ++addr;
            }
        }
    }
    end = std::chrono::high_resolution_clock::now();
    duration = std::chrono::duration_cast<std::chrono::milliseconds>(end-start);
    std::cout << "Create \"sorted\" list " << addresses.size() << " addresses " << duration.count() << " ms" << std::endl;
*/

    // std::vector<MemPlanner *> planners;

    // auto rom_data_planner = new MemPlanner(MEM_ROWS, 0x80000000, 128, false);
    // auto input_data_planner = new MemPlanner(MEM_ROWS, 0x90000000, 128, false);
    #define MAX_PLANNERS 4
    std::vector<MemPlanner> mem_planners;
    auto mem_planner = new MemPlanner(0, MEM_ROWS, 0xA0000000, 512);
    for (int i = 0; i < MAX_PLANNERS; ++i) {
        mem_planners.emplace_back(i+1, MEM_ROWS, 0xA0000000, 512);
    }

    MemLocators locators;
    std::vector<std::thread> planner_threads;

    start = std::chrono::high_resolution_clock::now();
    planner_threads.emplace_back([mem_planner, workers, &locators](){ mem_planner->generate_locators(workers, locators);});
    // planner_threads.emplace_back([rom_data_planner, workers, &locators](){ rom_data_planner->execute(workers);});
    // planner_threads.emplace_back([input_data_planner, workers, &locators](){ input_data_planner->execute(workers);});
    for (int i = 0; i < mem_planners.size(); ++i) {
        planner_threads.emplace_back([&mem_planners, i, workers, &locators]{ mem_planners[i].execute_from_locators(workers, locators);});
    }
    // planner_threads.emplace_back([mem_planner1, workers, &locators](){ mem_planner1->execute_from_locators(workers, locators);});
    // planner_threads.emplace_back([mem_planner2, workers, &locators](){ mem_planner2->execute_from_locators(workers, locators);});
    // planner_threads.emplace_back([mem_planner3, workers, &locators](){ mem_planner3->execute_from_locators(workers, locators);});
    // planner_threads.emplace_back([mem_planner4, workers, &locators](){ mem_planner4->execute_from_locators(workers, locators);});


    // for (int i = 0; i < planners.size(); ++i) {
    //     planner_threads.emplace_back([planners, workers, &locators, i](){ planners[i]->generate_locators(workers, locators);});
    //     //planner_threads.emplace_back([planners, workers, &locators, i](){ planners[i]->execute(workers);});
    // }
    for (auto& t : planner_threads) {
        t.join();
    }
    end = std::chrono::high_resolution_clock::now();
    duration = std::chrono::duration_cast<std::chrono::milliseconds>(end-start);
    std::cout << "Duration planners " << duration.count() << " ms" << std::endl;
    mem_planner->stats();

    for (int i = 0; i < mem_planners.size(); ++i) {
        mem_planners[i].stats();
    }

/*
    int count = 0;
    int tot_count = 0;
    int tot_count2 = 0;
    uint32_t zeros = 0;
    uint32_t non_zeros = 0;
    start = std::chrono::high_resolution_clock::now();
    // auto end2 = std::chrono::high_resolution_clock::now();
    for (int page = 0; page < MAX_PAGES; ++page) {
        uint32_t page_offset = page * ADDR_PAGE_SIZE;
        uint32_t offset = workers[0]->first_offset[page];
        uint32_t last_offset = workers[0]->last_offset[page];
        for (int i = 1; i < MAX_THREADS; ++i) {
        int page_offset = page * ADDR_PAGE_SIZE;
        uint32_t max_addr = workers[0]->last_addr[page];
        for (int i = 1; i < MAX_THREADS; ++i) {
            if (workers[i]->last_addr[page] > max_addr) {
                max_addr = workers[i]->last_addr[page];
            }
        }
        if (max_addr == 0) {
            continue;
        }
        uint32_t max_offset = MemCounter::addr_to_offset(max_addr) - page_offset;
        for (uint32_t index = 0; index <= max_offset; ++index) {
            uint32_t offset = page_offset + index;
            for (uint32_t i = 0; i < MAX_THREADS; ++i) {
                uint32_t pos = workers[i]->get_addr_table(offset);
                // printf("page: %d index: %d offset: %d => worker[%d] = pos: %d\n", page, index, offset, i, pos);
                if (pos != 0) {
                    ++count;
                    uint32_t initial_pos = workers[i]->get_initial_pos(pos);
                    uint32_t cpos = initial_pos;
                    uint32_t ptotal = 0;
                    // printf("index: %d => pos: %d => initial_pos: %d\n", index, pos, initial_pos);
                    while (cpos != 0) {
                        ++tot_count;
                        // uint32_t chunk_id = workers[i]->get_pos_value(cpos);
                        uint32_t cnt = workers[i]->get_pos_value(cpos+1);
                        ptotal += cnt;
                        // uint32_t _cpos = cpos;
                        if (cpos == pos) break;
                        cpos = workers[i]->get_next_pos(cpos+1);
                        // printf("pos: %d => cpos => %d\n", _cpos, cpos);
                    }
                    // printf("SUMMARY 0x%08X %d\n", 0x80000000 + (i + index * MAX_THREADS) * 8, ptotal);
                    tot_count2 += ptotal;
                    ++non_zeros;
                } else {
                    ++zeros;
                }
            }
        }
    }
*/

    // uint32_t i = (0x80283508 >> 3) & ((1 << THREAD_BITS) - 1);
    // uint32_t index = addr_to_offset(0x80283508);
    // uint32_t pos = workers[i]->get_addr_table(index);
    // if (pos != 0) {
    //     ++count;
    //     uint32_t initial_pos = workers[i]->get_initial_pos(pos);
    //     printf("pos: %d initial: %d\n", pos, initial_pos);
    //     uint32_t cpos = initial_pos;
    //     // printf("index: %d => pos: %d => initial_pos: %d\n", index, pos, initial_pos);
    //     while (cpos != 0) {
    //         ++tot_count;
    //         uint32_t chunk_id = workers[i]->get_pos_value(cpos);
    //         uint32_t cnt = workers[i]->get_pos_value(cpos+1);
    //         tot_count2 += cnt;
    //         printf("[%d]: (%d,%d)\n", cpos, chunk_id, cnt);
    //         if (cpos == pos) break;
    //         uint32_t _cpos = cpos;
    //         cpos = workers[i]->get_next_pos(cpos+1);
    //         printf("pos: %d => cpos => %d\n", _cpos, cpos);
    //     }
    // }

    end = std::chrono::high_resolution_clock::now();
    duration = std::chrono::duration_cast<std::chrono::milliseconds>(end-start);
    std::cout << "Duration plan " << duration.count() << " ms" << std::endl;
    // printf("Counted %d addresses, tot_count: %d, tot_count2:%d zeros: %u non_zeros: %u\n", count, tot_count, tot_count2, zeros, non_zeros);
/*
    count = 0;
    start = std::chrono::high_resolution_clock::now();
    // auto end2 = std::chrono::high_resolution_clock::now();
    for (int page = 0; page < MAX_PAGES; ++page) {
        int page_offset = page * ADDR_PAGE_SIZE;
        uint32_t max_addr = workers[0]->last_addr[page];
        for (int i = 1; i < MAX_THREADS; ++i) {
            if (workers[i]->last_addr[page] > max_addr) {
                max_addr = workers[i]->last_addr[page];
            }
        }
        if (max_addr == 0) {
            continue;
        }
        uint32_t max_offset = MemCounter::addr_to_offset(max_addr) - page_offset;
        for (uint32_t index = 0; index <= max_offset; ++index) {
            for (uint32_t i = 0; i < MAX_THREADS; ++i) {
                uint32_t pos = workers[i]->get_addr_table(index);
                if (pos != 0) {
                    ++count;
                }
            }
        }
    }*/
    end = std::chrono::high_resolution_clock::now();
    duration = std::chrono::duration_cast<std::chrono::milliseconds>(end-start);
    // std::cout << "Used " << count << " addresses " << ((double)count*100.0)/(double)(ADDR_TABLE_SIZE*MAX_THREADS) << "% " << duration.count() << "ms count:" << count << std::endl;


    return 0;
}
