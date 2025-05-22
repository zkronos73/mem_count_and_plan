#ifndef __MEM_COUNT_AND_PLAN_HPP__
#define __MEM_COUNT_AND_PLAN_HPP__

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
#include "tools.hpp"
#include "mem_counter.hpp"
#include "mem_align_counter.hpp"
#include "mem_planner.hpp"
#include "mem_locator.hpp"
#include "immutable_mem_planner.hpp"

#ifdef false
typedef struct {
    int thread_index;
    const MemCountTrace *mcp;
    int count;
} MemCountAndPlanThread;


class MemCountAndPlan {
private:
    MemLocators locators;
    MemCountTrace mcp;
    MemCountersBusData *chunk_data;
    uint32_t *chunk_size;
    uint32_t chunks;
    uint32_t max_chunks;
    std::vector<std::thread> planner_threads;
    std::vector<MemCounter *> workers;
    MemLocators locators;
public:

    MemCountAndPlan() {
        chunk_data = nullptr;
        chunk_size = nullptr;
    }
    ~MemCountAndPlan() {
    }
    void setup(uint32_t max_chunks = MAX_CHUNKS) {
        chunks = 0;
        chunk_data = (MemCountersBusData *)malloc(sizeof(MemCountersBusData) * max_chunks);
        chunk_size = (uint32_t *)malloc(sizeof(MemCountersBusData) * max_chunks);
        this->max_chunks = max_chunks;
    }
    void load() {
        chunks = 0;
        uint32_t tot_chunks = 0;
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
    }
    void execute(void) {
        count_phase();
        plan_phase();
        stats();
    }
    void count_phase() {
        auto start = std::chrono::high_resolution_clock::now();
        std::vector<std::thread> threads;

        uint64_t init = get_usec();
        for (size_t i = 0; i < MAX_THREADS; ++i) {
            MemCounter *th = new MemCounter(i, &mcp, mcp.chunk_size[i], init);
            workers.push_back(th);
        }
        auto mem_align_counter = new MemAlignCounter(MEM_ALIGN_ROWS, &mcp, init);
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end-start);

        std::cout << "Duration initialization " << duration.count() << " ms" << std::endl;

        start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < MAX_THREADS; ++i) {
            threads.emplace_back([this, i](){ workers[i]->execute();});
        }
        threads.emplace_back([mem_align_counter](){ mem_align_counter->execute();});

        for (auto& t : threads) {
            t.join();
        }

        end = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration_cast<std::chrono::milliseconds>(end-start);
        std::cout << "Mem count " << duration.count() << " ms" << std::endl;

        std::cout << "MemAlign " << mem_align_counter->get_instances_count() << " instances, on " << mem_align_counter->get_ellapsed_ms() << " ms" << std::endl;
    }

    void plan_phase() {

        // std::vector<MemPlanner *> planners;

        // auto rom_data_planner = new MemPlanner(MEM_ROWS, 0x80000000, 128, false);
        // auto input_data_planner = new MemPlanner(MEM_ROWS, 0x90000000, 128, false);
        std::vector<MemPlanner> mem_planners;
        auto mem_planner = new MemPlanner(0, MEM_ROWS, 0xA0000000, 512);
        for (int i = 0; i < MAX_MEM_PLANNERS; ++i) {
            mem_planners.emplace_back(i+1, MEM_ROWS, 0xA0000000, 512);
        }


        auto start = std::chrono::high_resolution_clock::now();
        planner_threads.emplace_back([mem_planner, this, &locators](){ mem_planner->generate_locators(workers, locators);});
        // planner_threads.emplace_back([rom_data_planner, workers, &locators](){ rom_data_planner->execute(workers);});
        // planner_threads.emplace_back([input_data_planner, workers, &locators](){ input_data_planner->execute(workers);});
        for (size_t i = 0; i < mem_planners.size(); ++i) {
            planner_threads.emplace_back([&mem_planners, i, this, &locators]{ mem_planners[i].execute_from_locators(workers, locators);});
        }

        for (auto& t : planner_threads) {
            t.join();
        }
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end-start);
        std::cout << "Mem plan " << duration.count() << " ms" << std::endl;
        mem_planner->stats();

        for (uint32_t i = 0; i < mem_planners.size(); ++i) {
            mem_planners[i].stats();
        }
    }
    void stats() {
        uint32_t tot_used_slots = 0;
        for (size_t i = 0; i < MAX_THREADS; ++i) {
            uint32_t used_slots = workers[i]->get_used_slots();
            tot_used_slots += used_slots;
            printf("Thread %ld: used slots %d/%d (%04.02f%%) T:%d ms S:%d ms Q:%d\n", i, used_slots, ADDR_SLOTS, ((double)used_slots*100.0)/(double)(ADDR_SLOTS), workers[i]->get_ellapsed_ms(), workers[i]->get_tot_usleep()/1000, workers[i]->get_queue_full_times()/1000);
        }
        printf("\n> threads: %d\n", MAX_THREADS);
        printf("> address table: %ld MB\n", (ADDR_TABLE_SIZE * ADDR_TABLE_ELEMENT_SIZE * MAX_THREADS)>>20);
        printf("> memory slots: %ld MB (used: %ld MB)\n", (ADDR_SLOTS_SIZE * sizeof(uint32_t) * MAX_THREADS)>>20, (tot_used_slots * ADDR_SLOT_SIZE * sizeof(uint32_t))>> 20);
        printf("> page table: %ld MB\n\n", (ADDR_PAGE_SIZE * sizeof(uint32_t))>> 20);
    }

};
#endif
#endif