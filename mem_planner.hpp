#ifndef __MEM_PLANNER_HPP__
#define __MEM_PLANNER_HPP__

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
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
#include "mem_segment.hpp"
#include "mem_check_point.hpp"
#include "mem_locators.hpp"
#include "mem_locator.hpp"

class MemPlanner {
private:
    uint32_t id;
    uint32_t rows;
    uint32_t from_page;
    uint32_t to_page;
    uint32_t rows_available;
    uint32_t reference_addr_chunk;
    uint32_t reference_addr;
    uint32_t reference_skip;
    uint32_t current_chunk;
    uint32_t last_addr;
    uint32_t locators_done;
    #ifndef MEM_CHECK_POINT_MAP
    uint32_t *chunk_table;
    uint32_t limit_pos;
    #endif
    #ifdef SEGMENT_STATS
    uint32_t max_chunks;
    uint32_t large_segments;
    uint32_t tot_chunks;
    #endif
    #ifdef DIRECT_MEM_LOCATOR
    MemLocator locators[MAX_CHUNKS];
    uint32_t locators_count;
    #endif
    MemSegment *current_segment;
    #ifdef MEM_PLANNER_STATS
    uint64_t locators_times[8];
    uint32_t locators_time_count;
    #endif

public:
    MemPlanner(uint32_t id, uint32_t rows, uint32_t from_addr, uint32_t mb_size)
    :id(id),rows(rows) {
        rows_available = rows;
        reference_addr_chunk = NO_CHUNK_ID;
        reference_addr = 0;
        reference_skip = 0;
        locators_done = 0;
        #ifdef MEM_PLANNER_STATS
        locators_time_count = 0;
        #endif

        current_chunk = NO_CHUNK_ID;
        #ifdef DIRECT_MEM_LOCATOR
        locators_count = 0;
        #endif
        current_segment = nullptr;
        from_page = MemCounter::addr_to_page(from_addr);
        to_page = MemCounter::addr_to_page(from_addr + (mb_size * 1024 * 1024) - 1);
        printf("MemPlanner: pages(%d-%d)\n", from_page, to_page);
        if (MemCounter::page_to_addr(from_page) != from_addr) {
            std::ostringstream msg;
            msg << "MemPlanner::constructor: from_addr " << std::hex << from_addr << " not aligned to page " << std::dec << from_page;
            throw std::runtime_error(msg.str());
        }
        #ifndef MEM_CHECK_POINT_MAP
        chunk_table = (uint32_t *)malloc(MAX_CHUNKS * sizeof(uint32_t));
        memset(chunk_table, 0, MAX_CHUNKS * sizeof(uint32_t));
        limit_pos = 0x00010000;
        #endif
        #ifdef SEGMENT_STATS
        max_chunks = 0;
        tot_chunks = 0;
        large_segments = 0;
        #endif
    }
    ~MemPlanner() {
    }
    const MemLocator *get_next_locator(MemLocators &locators, uint32_t us_timeout = 10) {
        const MemLocator *plocator = locators.get_locator();
        bool completed = false;
        while (plocator == nullptr) {
            if (completed && locators.is_completed()) {
                return nullptr;
            }
            plocator = locators.get_locator();
            if (plocator != nullptr) {
                return plocator;
            }
            completed = true;
            usleep(us_timeout);
            continue;
        }
        return plocator;
    }

    void execute_from_locators(const std::vector<MemCounter *> &workers, MemLocators &locators) {
        const MemLocator *locator;
        while (true) {
            if ((locator = get_next_locator(locators)) == nullptr) {
                break;
            }
            execute_from_locator(workers, locator);
        }
    }
    void execute_from_locator(const std::vector<MemCounter *> &workers, const MemLocator *locator) {
        uint32_t addr = 0;
        uint32_t max_offset = 0;
        ++locators_done;
        // printf("MemPlanner::execute_from_locator: (th:%d, o:%d, p:%d, s:%d)\n", locator->thread_index, locator->offset, locator->cpos, locator->skip);
        uint32_t skip = locator->skip;
        uint32_t offset = locator->offset;
        uint32_t page = MemCounter::offset_to_page(offset);
        uint32_t thread_index = locator->thread_index;
        for (;page < to_page; ++page, thread_index = 0, get_offset_limits(workers, page, offset, max_offset)) {
            addr = MemCounter::offset_to_addr(offset, thread_index);
            for (;offset <= max_offset; ++offset, thread_index = 0) {
                for (;thread_index < MAX_THREADS; ++thread_index, ++addr) {
                    uint32_t pos = workers[thread_index]->get_addr_table(offset);
                    if (pos == 0) continue;
                    uint32_t cpos = workers[thread_index]->get_initial_pos(pos);
                    while (cpos != 0) {
                        uint32_t chunk_id = workers[thread_index]->get_pos_value(cpos);
                        uint32_t count = workers[thread_index]->get_pos_value(cpos+1);
                        if (add_chunk(chunk_id, addr, count, skip) == false) {
                            return;
                        }
                        if (cpos == pos) break;
                        cpos = workers[thread_index]->get_next_pos(cpos+1);
                    }
                }
            }
        }
        /*
        #ifdef SEGMENT_STATS
        printf("MemPlanner::execute  segments:%d tot_chunks:%d max_chunks:%d large_segments:%d avg_chunks:%04.2f\n", segments.size(), tot_chunks, max_chunks, large_segments, ((double)tot_chunks)/((double)(segments.size())));
        #endif
        */
    }

    void generate_locators(const std::vector<MemCounter *> &workers, MemLocators &locators) {
        uint64_t init = get_usec();
        uint64_t elapsed = 0;
        rows_available = rows;
        uint32_t count;
        uint32_t offset, max_offset;
        bool inserted_first_locator = false;
        for (uint32_t page = from_page; page < to_page; ++page) {
            get_offset_limits(workers, page, offset, max_offset);
            for (;offset <= max_offset; ++offset) {
                for (uint32_t thread_index = 0; thread_index < MAX_THREADS; ++thread_index) {
                    uint32_t pos = workers[thread_index]->get_addr_table(offset);
                    if (pos == 0) continue;
                    if (inserted_first_locator) {
                        inserted_first_locator = true;
                        locators.push_locator(thread_index, offset, pos, 0);
                    }
                    uint32_t addr_count = workers[thread_index]->get_count_table(offset);
                    if (rows_available > addr_count) {
                        rows_available -= addr_count;
                        continue;
                    }
                    uint32_t cpos = workers[thread_index]->get_initial_pos(pos);
                    while (true) {
                        count = workers[thread_index]->get_pos_value(cpos+1);
                        while (count > 0) {
                            if (rows_available > count) {
                                rows_available -= count;
                                break;
                            } else if (rows_available <= count) {
                                // when rows_available == count, we need to pass by offset,cpos to get last value
                                #ifdef MEM_PLANNER_STATS
                                if (locators_time_count < 8) {
                                    locators_times[locators_time_count++] = get_usec() - init;
                                }
                                #endif
                                locators.push_locator(thread_index, offset, cpos, rows_available);
                                count -= rows_available;
                                rows_available = rows;
                            }
                        }
                        if (pos == cpos) break;
                        cpos = workers[thread_index]->get_next_pos(cpos+1);
                    }
                }
            }
        }
        locators.set_completed();
    }
    void get_offset_limits(const std::vector<MemCounter *> &workers, uint32_t page, uint32_t &first_offset, uint32_t &last_offset) {
        first_offset = workers[0]->first_offset[page];
        last_offset = workers[0]->last_offset[page];
        for (int i = 1; i < MAX_THREADS; ++i) {
            first_offset = std::min(first_offset, workers[i]->first_offset[page]);
            last_offset = std::min(last_offset, workers[i]->last_offset[page]);
        }
    }
    bool add_chunk(uint32_t chunk_id, uint32_t addr, uint32_t count, uint32_t skip = 0) {
        if (current_segment == nullptr) {
            // include first chunk
            uint32_t consumed = std::min(count - skip, rows_available);
            current_segment = new MemSegment(chunk_id, addr, skip, consumed);
            rows_available -= consumed;
            return (rows_available != 0);
        }
        if (rows_available <= count) {
            current_segment->add_or_update(chunk_id, addr, rows_available);
            return false;
        }
        current_segment->add_or_update(chunk_id, addr, count);
        rows_available -= count;
        return true;
    }

    void stats() {
        printf("MemPlanner[%d]::stats: locators_done:%d\n", id, locators_done);
        #ifdef MEM_PLANNER_STATS
        for (uint32_t index = 0; index < locators_time_count; ++index) {
            printf("MemPlanner::stats: locators_time[%d]: %lu\n", index, locators_times[index]);
        }
        #endif
    }
    uint64_t *get_locators_times(uint32_t &count) {
        #ifdef MEM_PLANNER_STATS
        count = locators_time_count;
        return locators_times;
        #else
        count = 0;
        return nullptr;
        #endif
    }
};
#endif