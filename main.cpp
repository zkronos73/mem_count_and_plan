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

#include "mem_types.hpp"
#include "mem_config.hpp"
#include "tools.hpp"
#include "mem_counter.hpp"
#include "mem_align_counter.hpp"

#define CHUNK_MAX_DISTANCE 16
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

class MemCheckPoint {
    private:
        #ifndef MEM_CHECK_POINT_MAP
        uint32_t chunk_id;
        #endif
        uint32_t from_addr;
        uint32_t from_skip;
        uint32_t to_addr;
        uint32_t to_count;
        uint32_t count;
        uint32_t intermediate_skip;
    public:
        #ifdef MEM_CHECK_POINT_MAP
        MemCheckPoint(uint32_t from_addr, uint32_t skip, uint32_t count, uint32_t intermediate_skip) :
        #else
        MemCheckPoint(uint32_t chunk_id, uint32_t from_addr, uint32_t skip, uint32_t count, uint32_t intermediate_skip) : chunk_id(chunk_id),
        #endif
            from_addr(from_addr),
            to_addr(from_addr),
            to_count(count),
            count(count),
            intermediate_skip(intermediate_skip) {
        }
        ~MemCheckPoint() {
        }
        void add_rows(uint32_t addr, uint32_t count) {
            this->count += count;
            if (addr == to_addr) {
                to_count += count;
            } else {
                to_addr = addr;
                to_count = count;
            }
        }
};


class MemSegment {
public:
    #ifdef MEM_CHECK_POINT_MAP
    std::unordered_map<uint32_t, MemCheckPoint> chunks;
    #else
    std::vector<MemCheckPoint> chunks;
    #endif
    bool is_last_segment;
    MemSegment() : is_last_segment(false) {
        chunks.reserve(1024);
    }
};

#define NO_CHUNK_ID 0xFFFFFFFF
class MemPlanner {
private:
    uint32_t rows;
    uint32_t from_page;
    uint32_t to_page;
    uint32_t rows_available;
    uint32_t reference_addr_chunk;
    uint32_t reference_addr;
    uint32_t reference_skip;
    uint32_t last_chunk;
    uint32_t current_chunk;
    uint32_t last_addr;
    #ifndef MEM_CHECK_POINT_MAP
    uint32_t *chunk_table;
    uint32_t limit_pos;
    #endif
    #ifdef SEGMENT_STATS
    uint32_t max_chunks;
    uint32_t large_segments;
    uint32_t tot_chunks;
    #endif
    bool intermediate_step_reads;
    MemSegment *current_segment;
    std::vector<MemSegment *> segments;

public:
    MemPlanner(uint32_t rows, uint32_t from_addr, uint32_t mb_size, bool intermediate_step_reads)
    :rows(rows), intermediate_step_reads(intermediate_step_reads) {
        rows_available = rows;
        reference_addr_chunk = NO_CHUNK_ID;
        reference_addr = 0;
        reference_skip = 0;
        last_chunk = NO_CHUNK_ID;
        current_chunk = NO_CHUNK_ID;
        current_segment = new MemSegment();
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
    void execute(const std::vector<MemCounter *> &workers) {
        uint32_t max_addr;
        uint32_t addr = 0;
        for (uint32_t page = from_page; page < to_page; ++page) {
            if (!(max_addr = get_max_addr(workers, page))) continue;
            addr = MemCounter::page_to_addr(page);
            uint32_t max_offset = MemCounter::addr_to_offset(max_addr);
            for (uint32_t offset = page * ADDR_PAGE_SIZE; offset <= max_offset; ++offset) {
                for (uint32_t i = 0; i < MAX_THREADS; ++i, ++addr) {
                    uint32_t pos = workers[i]->get_addr_table(offset);
                    if (pos == 0) continue;
                    uint32_t cpos = workers[i]->get_initial_pos(pos);
                    while (cpos != 0) {
                        uint32_t chunk_id = workers[i]->get_pos_value(cpos);
                        uint32_t count = workers[i]->get_pos_value(cpos+1);
                        add_to_current_segment(chunk_id, addr, count);
                        if (cpos == pos) break;
                        cpos = workers[i]->get_next_pos(cpos+1);
                    }
                }
            }
        }
        close_last_segment();
        #ifdef SEGMENT_STATS
        printf("MemPlanner::execute  segments:%d tot_chunks:%d max_chunks:%d large_segments:%d avg_chunks:%04.2f\n", segments.size(), tot_chunks, max_chunks, large_segments, ((double)tot_chunks)/((double)(segments.size())));
        #endif
    }
    uint32_t get_max_addr(const std::vector<MemCounter *> &workers, uint32_t page) {
        uint32_t max_addr = workers[0]->last_addr[page];
        for (int i = 1; i < MAX_THREADS; ++i) {
            if (workers[i]->last_addr[page] > max_addr) {
                max_addr = workers[i]->last_addr[page];
            }
        }
        return max_addr;
    }
    void add_to_current_segment(uint32_t chunk_id, uint32_t addr, uint32_t count) {
        set_current_chunk(chunk_id);
        uint32_t intermediate_rows = add_intermediates(addr);
        preopen_segment(addr, intermediate_rows);
        set_reference(chunk_id, addr);
        add_rows(addr, count);
    }
    void set_reference(uint32_t chunk_id, uint32_t addr) {
        reference_addr_chunk = chunk_id;
        reference_addr = addr;
        reference_skip = 0;
    }
    void set_current_chunk(uint32_t chunk_id) {
        last_chunk = current_chunk;
        current_chunk = chunk_id;
    }
    void close_last_segment() {
        if (rows_available < rows) {
            close_segment(true);
        } else if (segments.size() > 0) {
            segments.back()->is_last_segment = true;
        }
    }
    void close_segment(bool last = false) {
        current_segment->is_last_segment = last;
        // printf("MemPlanner::close_segment: %d chunks from_page:%d\n", current_segment->chunks.size(), from_page);
        #ifdef SEGMENT_STATS
        uint32_t segment_chunks = current_segment->chunks.size();
        if (segment_chunks > max_chunks) {
            max_chunks = segment_chunks;
        }
        if (segment_chunks >= SEGMENT_LARGE_CHUNKS) {
            large_segments++;
        }
        tot_chunks += segment_chunks;
        #endif

        segments.push_back(current_segment);
        current_segment = new MemSegment();
    }
    void open_segment(uint32_t intermediate_skip) {
        #ifndef MEM_CHECK_POINT_MAP
        limit_pos = (segments.size() + 1) << 16;
        #endif
        close_segment(false);
        if (reference_addr_chunk != NO_CHUNK_ID) {
            #ifdef MEM_CHECK_POINT_MAP
            current_segment->chunks.try_emplace(reference_addr_chunk, reference_addr, reference_skip, 0, intermediate_skip);
            #else
            current_segment->chunks.emplace_back(reference_addr_chunk, reference_addr, reference_skip, 0, intermediate_skip);
            #endif
        }
        rows_available = rows;
        // printf("MemPlanner::open_segment: rows_available: %d from_page:%d\n", rows_available, from_page);
    }
    void add_next_addr_to_segment(uint32_t addr) {
        add_chunk_to_segment(current_chunk, addr, 1, 0);
    }
    void add_chunk_to_segment(uint32_t chunk_id, uint32_t addr, uint32_t count, uint32_t skip) {
        #ifdef MEM_CHECK_POINT_MAP
        auto it = current_segment->chunks.find(chunk_id);
        if (it != current_segment->chunks.end()) {
            it->second.add_rows(addr, count);
        } else {
            current_segment->chunks.try_emplace(chunk_id, addr, skip, count, 0);
        }
        #else
        uint32_t pos = chunk_table[chunk_id];
        if (pos < limit_pos) {
            // not found
            // printf("MemPlanner::add_chunk_to_segment: chunk_id %d not found (pos: 0x%08X) size:%d limit_pos: 0x%08X\n", chunk_id, pos, current_segment->chunks.size(), limit_pos);
            chunk_table[chunk_id] = limit_pos + current_segment->chunks.size();
            current_segment->chunks.emplace_back(chunk_id, addr, skip, count, 0);
        } else {
            uint32_t vpos = pos & 0xFFFF;
            // printf("MemPlanner::add_chunk_to_segment: chunk_id %d already exists at vpos %d (pos: 0x%08X) size:%d limit_pos: 0x%08X\n", chunk_id, vpos, pos, current_segment->chunks.size(), limit_pos);
            current_segment->chunks[vpos].add_rows(addr, count);
        }
        #endif
    }
    void preopen_segment(uint32_t addr, uint32_t intermediate_rows) {
        if (rows_available == 0) {
            if (intermediate_rows > 0) {
                add_next_addr_to_segment(addr);
            }
            open_segment(intermediate_rows);
        }
    }
    void consume_rows(uint32_t addr, uint32_t row_count, uint32_t skip) {
        if (row_count == 0 && rows_available > 0) {
            return;
        }
        if (row_count > rows_available) {
            std::ostringstream msg;
            msg << "MemPlanner::consume " << row_count << " too much rows, available " << rows_available << std::endl;
            throw std::runtime_error(msg.str());
        }
        if (rows_available == 0) {
            open_segment(0);
        }
        add_chunk_to_segment(current_chunk, addr, row_count, skip);
        rows_available -= row_count;
        reference_skip += row_count;
    }

    void consume_intermediate_rows(uint32_t addr, uint32_t row_count, uint32_t skip) {
        if (row_count == 0 && rows_available > 0) {
            return;
        }
        if (row_count > rows_available) {
            std::ostringstream msg;
            msg << "MemPlanner::consume " << row_count << " too much rows, available " << rows_available << std::endl;
            throw std::runtime_error(msg.str());
        }
        if (rows_available == 0) {
            open_segment(0);
        }
        if (intermediate_step_reads) {
            add_chunk_to_segment(current_chunk, addr, rows, skip);
        }
        rows_available -= row_count;
    }

    void add_intermediate_rows(uint32_t addr, uint32_t count) {
        uint32_t pending = count;
        while (pending > 0) {
            uint32_t rows = std::min(pending, rows_available);
            uint32_t skip = count - pending;
            consume_intermediate_rows(addr, rows, skip);
            pending -= rows;
        }
    }

    void add_rows(uint32_t addr, uint32_t count) {
        uint32_t pending = count;
        while (pending > 0) {
            uint32_t rows = std::min(pending, rows_available);
            uint32_t skip = count - pending;
            consume_rows(addr, rows, skip);
            pending -= rows;
        }
    }

    void add_intermediate_addr(uint32_t from_addr, uint32_t to_addr) {
        // adding internal reads of zero for consecutive addresses
        uint32_t count = to_addr - from_addr + 1;
        if (count > 1) {
            add_intermediate_rows(from_addr, 1);
            add_intermediate_rows(to_addr, count - 1);
        } else {
            add_intermediate_rows(to_addr, 1);
        }
    }

    uint32_t add_intermediates(uint32_t addr) {
        if (last_addr != addr) {
            if (!intermediate_step_reads && (addr - last_addr) > 1) {
                add_intermediate_addr(last_addr + 1, addr - 1);
            }
            last_addr = addr;
        } else if (intermediate_step_reads) {
            return add_intermediate_steps(addr);
        }
        return 0;
    }

    uint32_t add_intermediate_steps(uint32_t addr) {
        // check if the distance between the last chunk and the current is too large,
        // if so then we need to add intermediate rows
        uint32_t intermediate_rows = 0;
        if (last_chunk != NO_CHUNK_ID) {
            uint32_t chunk_distance = current_chunk - last_chunk;
            if (chunk_distance > CHUNK_MAX_DISTANCE) {
                this->add_intermediate_rows(addr, 1);
            }
        }
        return intermediate_rows;
    }
};

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

    // Esperar a que tots els threads acabin
    for (auto& t : threads) {
        t.join();
    }
    end = std::chrono::high_resolution_clock::now();
    duration = std::chrono::duration_cast<std::chrono::milliseconds>(end-start);
    std::cout << "Duration count & sort " << duration.count() << " ms" << std::endl;

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
        printf("Thread %d: used slots %d/%d (%04.02f%%) T:%d ms\n", i, used_slots, ADDR_SLOTS, ((double)used_slots*100.0)/(double)(ADDR_SLOTS), workers[i]->get_ellapsed_ms());
    }
    printf("\n> threads: %d\n", MAX_THREADS);
    printf("> address table: %ld MB\n", (ADDR_TABLE_SIZE * sizeof(uint32_t) * MAX_THREADS)>>20);
    printf("> memory slots: %ld MB (used: %ld MB)\n", (ADDR_SLOTS_SIZE * sizeof(uint32_t) * MAX_THREADS)>>20, (tot_used_slots * ADDR_SLOT_SIZE * sizeof(uint32_t))>> 20);
    printf("> page table: %ld MB\n\n", (ADDR_PAGE_SIZE * sizeof(uint32_t))>> 20);

    std::vector<MemPlanner *> planners;

    start = std::chrono::high_resolution_clock::now();
    // planners.push_back(new MemPlanner(MEM_ROWS, 0x80000000, 128, false));
    // planners.push_back(new MemPlanner(MEM_ROWS, 0x90000000, 128, false));
    planners.push_back(new MemPlanner(MEM_ROWS, 0xA0000000, 512, true));

    std::vector<std::thread> planner_threads;
    for (int i = 0; i < planners.size(); ++i) {
        planner_threads.emplace_back([planners, workers, i](){ planners[i]->execute(workers);});
    }
    for (auto& t : planner_threads) {
        t.join();
    }
    end = std::chrono::high_resolution_clock::now();
    duration = std::chrono::duration_cast<std::chrono::milliseconds>(end-start);
    std::cout << "Duration planners " << duration.count() << " ms" << std::endl;

    int count = 0;
    int tot_count = 0;
    int tot_count2 = 0;
    uint32_t zeros = 0;
    uint32_t non_zeros = 0;
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
    printf("Counted %d addresses, tot_count: %d, tot_count2:%d zeros: %u non_zeros: %u\n", count, tot_count, tot_count2, zeros, non_zeros);

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
    }
    end = std::chrono::high_resolution_clock::now();
    duration = std::chrono::duration_cast<std::chrono::milliseconds>(end-start);
    std::cout << "Used " << count << " addresses " << ((double)count*100.0)/(double)(ADDR_TABLE_SIZE*MAX_THREADS) << "% " << duration.count() << "ms" << std::endl;


    return 0;
}
