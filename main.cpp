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

typedef struct {
    uint32_t addr;
    uint32_t flags;
} MemCountersBusData;

struct MemAlignCount {
    uint32_t chunk_id;
    uint32_t count[3];
    MemAlignCount(uint32_t chunk_id, uint32_t count[3]) : chunk_id(chunk_id), count{count[0], count[1], count[2]} {}
};

#define MAX_CHUNKS 4096

typedef struct {
    // BusDataChunk *chunk_data[MAX_CHUNKS];
    MemCountersBusData *chunk_data[MAX_CHUNKS];
    int chunk_size[MAX_CHUNKS];
    int chunks = 0;
} MemCountAndPlan;

typedef struct {
    int thread_index;
    const MemCountAndPlan *mcp;
    int count;
} MemCountAndPlanThread;

#define THREAD_BITS 3
#define ADDR_LOW_BITS (THREAD_BITS + 5)
#define MAX_THREADS (1 << THREAD_BITS)
#define ADDR_MASK ((MAX_THREADS - 1) * 8)

#define MAX_PAGES 20
#define ADDR_PAGE_BITS (23 - THREAD_BITS)
#define ADDR_PAGE_SIZE (1 << ADDR_PAGE_BITS)
#define ADDR_TABLE_SIZE (ADDR_PAGE_SIZE * MAX_PAGES)
#define OFFSET_BITS (25 + 4 - THREAD_BITS) // 4 bits (3 bits for 6 pages, 1 bit security)
#define OFFSET_PAGE_SHIFT_BITS (OFFSET_BITS - 3)

#define ADDR_SLOT_BITS 5
#define ADDR_SLOT_SIZE (1 << ADDR_SLOT_BITS)
#define ADDR_SLOT_MASK (0xFFFFFFFF << ADDR_SLOT_BITS)
#define ADDR_SLOTS ((1024 * 1024 * 8) / MAX_THREADS)

#define ADDR_SLOTS_SIZE (ADDR_SLOT_SIZE * ADDR_SLOTS)
#define TIME_US_BY_CHUNK 250

inline uint64_t get_usec() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

int load_from_compact_file(size_t chunk_id, MemCountersBusData** chunk) {
    char filename[256];
    snprintf(filename, sizeof(filename), "../bus_data/mem_count_data/mem_count_data_%ld.bin", chunk_id);
    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("Error getting file size");
        close(fd);
        return -1;
    }
    int chunk_size = st.st_size / sizeof(MemCountersBusData);
    int size = sizeof(MemCountersBusData) * chunk_size;
    *chunk = (MemCountersBusData *)malloc(size);
    if (*chunk == NULL) {
        perror("Error allocating memory");
        close(fd);
        return -1;
    }
    ssize_t bytes_read = read(fd, *chunk, size);
    if (bytes_read < 0) {
        perror("Error reading file");
        free(*chunk);
        close(fd);
        return -1;
    }
    close(fd);
    if (bytes_read != size) {
        fprintf(stderr, "Warning: Read %zd bytes, expected %d bytes\n", bytes_read, size);
    }
    return chunk_size;
}

inline uint32_t addr_to_offset(uint32_t addr, uint32_t chunk_id = 0, uint32_t index = 0) {
    switch((uint8_t)((addr >> 24) & 0xFC)) {
        case 0x80: return ((addr - 0x80000000) >> (ADDR_LOW_BITS));
        case 0x84: return ((addr - 0x84000000) >> (ADDR_LOW_BITS)) + ADDR_PAGE_SIZE;
        case 0x90: return ((addr - 0x90000000) >> (ADDR_LOW_BITS)) + 2 * ADDR_PAGE_SIZE;
        case 0x94: return ((addr - 0x94000000) >> (ADDR_LOW_BITS)) + 3 * ADDR_PAGE_SIZE;
        case 0xA0: return ((addr - 0xA0000000) >> (ADDR_LOW_BITS)) + 4 * ADDR_PAGE_SIZE;
        case 0xA4: return ((addr - 0xA4000000) >> (ADDR_LOW_BITS)) + 5 * ADDR_PAGE_SIZE;
        case 0xA8: return ((addr - 0xA8000000) >> (ADDR_LOW_BITS)) + 6 * ADDR_PAGE_SIZE;
        case 0xAC: return ((addr - 0xAC000000) >> (ADDR_LOW_BITS)) + 7 * ADDR_PAGE_SIZE;
        case 0xB0: return ((addr - 0xB0000000) >> (ADDR_LOW_BITS)) + 8 * ADDR_PAGE_SIZE;
        case 0xB4: return ((addr - 0xB4000000) >> (ADDR_LOW_BITS)) + 9 * ADDR_PAGE_SIZE;
        case 0xB8: return ((addr - 0xB8000000) >> (ADDR_LOW_BITS)) + 10 * ADDR_PAGE_SIZE;
        case 0xBC: return ((addr - 0xBC000000) >> (ADDR_LOW_BITS)) + 11 * ADDR_PAGE_SIZE;
        case 0xC0: return ((addr - 0xC0000000) >> (ADDR_LOW_BITS)) + 12 * ADDR_PAGE_SIZE;
        case 0xC4: return ((addr - 0xC4000000) >> (ADDR_LOW_BITS)) + 13 * ADDR_PAGE_SIZE;
        case 0xC8: return ((addr - 0xC8000000) >> (ADDR_LOW_BITS)) + 14 * ADDR_PAGE_SIZE;
        case 0xCC: return ((addr - 0xCC000000) >> (ADDR_LOW_BITS)) + 15 * ADDR_PAGE_SIZE;
        case 0xD0: return ((addr - 0xD0000000) >> (ADDR_LOW_BITS)) + 16 * ADDR_PAGE_SIZE;
        case 0xD4: return ((addr - 0xD4000000) >> (ADDR_LOW_BITS)) + 17 * ADDR_PAGE_SIZE;
        case 0xD8: return ((addr - 0xD8000000) >> (ADDR_LOW_BITS)) + 18 * ADDR_PAGE_SIZE;
        case 0xDC: return ((addr - 0xDC000000) >> (ADDR_LOW_BITS)) + 19 * ADDR_PAGE_SIZE;
    }
    printf("Error: addr_to_offset: 0x%X (%d:%d)\n", addr, chunk_id, index);
    exit(1);
}

inline uint32_t addr_to_page(uint32_t addr, uint32_t chunk_id = 0, uint32_t index = 0) {
    switch((uint8_t)((addr >> 24) & 0xFC)) {
        case 0x80: return 0;
        case 0x84: return 1;
        case 0x90: return 2;
        case 0x94: return 3;
        case 0xA0: return 4;
        case 0xA4: return 5;
        case 0xA8: return 6;
        case 0xAC: return 7;
        case 0xB0: return 8;
        case 0xB4: return 9;
        case 0xB8: return 10;
        case 0xBC: return 11;
        case 0xC0: return 12;
        case 0xC4: return 13;
        case 0xC8: return 14;
        case 0xCC: return 15;
        case 0xD0: return 16;
        case 0xD4: return 17;
        case 0xD8: return 18;
        case 0xDC: return 19;
    }
    printf("Error: addr_to_page: 0x%X (%d:%d)\n", addr, chunk_id, index);
    exit(1);
}

class MemCounter {
private:
    const int id_;
    const MemCountAndPlan *mcp;

    const int mcp_count;
    int count;
    int addr_count;
    uint32_t *addr_table;
    uint32_t *addr_slots;
    uint32_t current_index;
    uint32_t current_chunk;
    uint32_t free_slot;
    uint64_t init;
    uint32_t ellapsed_ms;
    uint64_t extra_unaligned;
    uint32_t mem_align_count[3];
    std::vector<MemAlignCount *> mem_align_counters;
    // static std::mutex cout_mutex_;
public:
    uint32_t last_addr[MAX_PAGES];
    MemCounter(int id, const MemCountAndPlan *mcp, int count, uint64_t init) : id_(id), mcp(mcp), mcp_count(count), init(init) {
        count = 0;
        addr_table = (uint32_t *)malloc(ADDR_TABLE_SIZE * sizeof(uint32_t));
        memset(addr_table, 0, ADDR_TABLE_SIZE * sizeof(uint32_t));

        addr_slots = (uint32_t *)malloc(ADDR_SLOTS_SIZE * sizeof(uint32_t));
        // memset(addr_slots, 0, ADDR_SLOTS_SIZE * sizeof(uint32_t));
        free_slot = 0;
        addr_count = 0;
        extra_unaligned = 0;
        mem_align_count[0] = 0;
        mem_align_count[1] = 0;
        mem_align_count[2] = 0;
    }
    uint32_t get_count() {
        printf("Thread %d: count %d addr_count %d\n", id_, count, addr_count);
        return addr_count;
    }
    uint32_t get_extra_unaligned() {
        return extra_unaligned;
    }
    uint32_t get_used_slots() {
        return free_slot;
    }
    ~MemCounter() {
        free(addr_table);
        free(addr_slots);
    }
    void execute() {
        const int chunks = mcp->chunks;
        const uint32_t mask_value = id_ * 8;

        for (int j = 0; j < chunks; ++j) {
            const uint32_t chunk_size = mcp->chunk_size[j];
            const MemCountersBusData *chunk_data = mcp->chunk_data[j];
            current_chunk = j;
            init_chunk(j);
            for (uint32_t i = 0; i < chunk_size; i++) {
                current_index = i;
                if (i >= chunk_size) {
                    return;
                }
                const uint8_t bytes = chunk_data[i].flags & 0xFF;
                const uint32_t addr = chunk_data[i].addr;
                if (bytes == 8 && (addr & 0x07) == 0) {
                    // aligned access
                    if ((addr & ADDR_MASK) != mask_value) {
                        continue;
                    }
                    count_aligned(addr, j, 1);
                } else {
                    const uint32_t aligned_addr = addr & 0xFFFFFFF8;

                    if ((aligned_addr & ADDR_MASK) == mask_value) {
                        const int ops = 1 + (chunk_data[i].flags >> 16);
                        extra_unaligned += ops;
                        count_aligned(aligned_addr, j, ops);
                        add_mem_align(ops);
                    }
                    else if ((bytes + (addr & 0x07)) > 8 && ((aligned_addr + 8) & ADDR_MASK) == mask_value) {
                        const int ops = 1 + (chunk_data[i].flags >> 16);
                        extra_unaligned += ops;
                        count_aligned(aligned_addr + 8 , j, ops);
                        add_mem_align(ops*2);
                    }
                }
            }
            close_chunk(j);
            uint64_t next_chunk = init + (uint64_t)(j+1) * TIME_US_BY_CHUNK;
            uint64_t current = get_usec();
            if (current < next_chunk) {
                usleep(next_chunk - current);
            }
        }
        ellapsed_ms = ((get_usec() - init) / 1000);
    }
    inline uint32_t get_initial_pos(uint32_t pos) {
        uint32_t tpos = pos & ADDR_SLOT_MASK;
        while (addr_slots[tpos] != 0) {
            tpos = addr_slots[tpos];
        }
        return tpos + 2;
    }
    void init_chunk(uint32_t chunk_id) {
        mem_align_count[0] = 0;
        mem_align_count[1] = 0;
        mem_align_count[2] = 0;
    }
    void close_chunk(uint32_t chunk_id) {
        uint32_t count = mem_align_count[0] + mem_align_count[1] + mem_align_count[2];
        if (count > 0) {
            auto item = new MemAlignCount(chunk_id, mem_align_count);
            mem_align_counters.push_back(item);
        }
    }

    inline uint32_t get_pos_value(uint32_t pos) {
        return addr_slots[pos];
    }
    inline uint32_t get_next_pos(uint32_t pos) {
        int relative_pos = pos & (ADDR_SLOT_SIZE - 1);
        if (relative_pos < (ADDR_SLOT_SIZE - 1)) {
            return pos + 1;
        }
        uint32_t tpos = pos & ADDR_SLOT_MASK;
        if (addr_slots[tpos+1] != 0) {
            return addr_slots[tpos+1]+2;
        }
        return 0;
    }
    inline uint32_t get_addr_table(uint32_t index) {
        return addr_table[index];
    }
    inline uint32_t get_next_slot_pos() {
        // if (free_slot >= ADDR_SLOTS) {
        //     printf("Error: no more free slots on thread %d\n", id_);
        //     exit(1);
        //     // return 0;
        // }
        return (free_slot++) * ADDR_SLOT_SIZE;
    }
    inline uint32_t align_row_count_to_index(uint32_t count) {
        switch (count) {
            case 1: return 0;
            case 2: return 1;
            case 4: return 2;
        }
        exit(1);
        return 0;
    }
    inline void add_mem_align(uint32_t count) {
        uint32_t index = align_row_count_to_index(count);
        mem_align_count[index] += count;
    }
    inline void count_aligned(uint32_t addr, uint32_t chunk_id, uint32_t count) {
        uint32_t offset = addr_to_offset(addr, current_chunk, current_index);
        // if (offset >= ADDR_TABLE_SIZE) {
        //     printf("Error: offset %d out of bounds for addr 0x%X\n", offset, addr);
        //     return;
        // }
        uint32_t pos = addr_table[offset];
        if (pos == 0) {
            uint32_t pos = get_next_slot_pos();
            addr_slots[pos] = 0;
            addr_slots[pos + 1] = 0;
            addr_slots[pos + 2] = chunk_id;
            addr_slots[pos + 3] = count;
            addr_table[offset] = pos + 2;
            uint32_t page = offset >> ADDR_PAGE_BITS;
            if (last_addr[page] < addr) {
                last_addr[page] = addr;
            }
            ++addr_count;
        } else {
            if (addr_slots[pos] == chunk_id) {
                addr_slots[pos + 1] += count;
                return;
            }
            if ((pos % ADDR_SLOT_SIZE) == (ADDR_SLOT_SIZE - 2)) {
                uint32_t npos = get_next_slot_pos();
                uint32_t tpos = pos - ADDR_SLOT_SIZE + 2;
                addr_slots[tpos + 1] = npos;
                addr_slots[npos] = tpos;
                addr_slots[npos + 1] = 0;
                addr_slots[npos + 2] = chunk_id;
                addr_slots[npos + 3] = count;
                addr_table[offset] = npos + 2;
                return;
            }
            addr_slots[pos + 2] = chunk_id;
            addr_slots[pos + 3] = count;
            addr_table[offset] = pos + 2;
        }
    }
    uint32_t get_ellapsed_ms() {
        return ellapsed_ms;
    }
};


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
    const MemCountAndPlan *mcp;
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
    MemAlignCounter(uint32_t rows, const MemCountAndPlan *mcp, uint64_t init) : rows(rows), mcp(mcp), init(init) {
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
    void execute() {

        const int chunks = mcp->chunks;
        for (int j = 0; j < chunks; ++j) {
            const uint32_t chunk_size = mcp->chunk_size[j];
            const MemCountersBusData *chunk_data = mcp->chunk_data[j];
            for (uint32_t i = 0; i < chunk_size; i++) {
                const uint8_t bytes = chunk_data[i].flags & 0xFF;
                const uint32_t addr = chunk_data[i].addr;
                if (bytes != 8 || (addr & 0x07) != 0) {
                    uint32_t ops = ((bytes + (addr & 0x07)) > 8) ? 2:1 * (1 + chunk_data[i].flags >> 16) + 1;
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
    inline void add_mem_align_op(uint32_t chunk_id, uint32_t ops) {
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




uint32_t count_operations(uint32_t chunk_id, MemCountersBusData *chunk_data, int count) {
    uint32_t ops = 0;
    uint32_t cops = 0;
    for (int i = 0; i < count; ++i) {
        const uint32_t bytes = chunk_data[i].flags & 0x0F;
        const uint32_t offset = chunk_data[i].addr & 0x07;
        const bool wr = (chunk_data[i].flags & 0x10000) != 0;
        if (offset == 0 && bytes == 8) {
            cops = 1;
        } else if (offset + bytes > 8) {
            if (wr) {
                cops = 4;
            } else {
                cops = 2;
            }
        } else if (wr) {
            cops = 2;
        } else {
            cops = 1;
        }
        ops += cops;
    }
    return ops;
}

class MemCheckpoint {
private:
    bool consecutive_address;
    uint32_t last_addr_w;
    uint32_t rows;
    uint32_t skip_rows;
    uint32_t rows_available;
public:
    MemCheckpoint(uint32_t from_address, uint32_t rows, bool consecutive_address): consecutive_address(consecutive_address), rows(rows) {
        // Constructor code here
        last_addr_w = from_address >> 3;
        skip_rows = 0;
        rows_available = rows;
    }
    ~MemCheckpoint() {
        // Destructor code here
    }
    void add(uint32_t chunk_id, uint32_t addr, uint32_t count) {
        uint32_t addr_w = addr >> 3;
        if (consecutive_address) {
            uint32_t inc_addr_w = addr_w - last_addr_w;
            if (inc_addr_w < 2) {
                rows_available -= 1;
            } else if (rows_available <= inc_addr_w) {
                rows_available -= inc_addr_w;
            } else {
                skip_rows = rows_available;
                rows_available = 0;
                close_instance();
                rows_available = rows;
            }
            last_addr_w = addr_w;
        } else {
            rows_available = 1;
            last_addr_w = addr_w;
        }
    }
    void close_instance() {
        // Close the instance and save the data
        // printf("Closing instance with last_addr: 0x%X\n", last_addr);
        // Save the data to a file or perform any other necessary operations
    }
};

int main() {
    printf("Starting...\n");

    MemCountAndPlan mcp;
    int chunks = 0;
    int tot_chunks = 0;
    uint32_t tot_ops = 0;
    printf("Loading compact data...\n");
    while (chunks < MAX_CHUNKS && (mcp.chunk_size[chunks] = load_from_compact_file(chunks, &(mcp.chunk_data[chunks]))) >=0) {
        tot_ops += count_operations(chunks, mcp.chunk_data[chunks], mcp.chunk_size[chunks]);
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
    auto mem_align_counter = new MemAlignCounter(1 << 22, &mcp, init);
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end-start);

    std::cout << "Duration initialization " << duration.count() << " ms" << std::endl;

    start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < MAX_THREADS; ++i) {
        threads.emplace_back([workers, i](){ workers[i]->execute();});
    }
    threads.emplace_back([mem_align_counter](){ mem_align_counter->execute();});
    end = std::chrono::high_resolution_clock::now();

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
        printf("Thread %d: used slots %d/%d (%04.02f%%) T:%ld ms\n", i, used_slots, ADDR_SLOTS, ((double)used_slots*100.0)/(double)(ADDR_SLOTS), workers[i]->get_ellapsed_ms());
    }
    printf("\n> threads: %d\n", MAX_THREADS);
    printf("> address table: %ld MB\n", (ADDR_TABLE_SIZE * sizeof(uint32_t) * MAX_THREADS)>>20);
    printf("> memory slots: %ld MB (used: %ld MB)\n", (ADDR_SLOTS_SIZE * sizeof(uint32_t) * MAX_THREADS)>>20, (tot_used_slots * ADDR_SLOT_SIZE * sizeof(uint32_t))>> 20);
    printf("> page table: %ld MB\n\n", (ADDR_PAGE_SIZE * sizeof(uint32_t))>> 20);

    int count = 0;
    int tot_count = 0;
    int tot_count2 = 0;
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
        uint32_t max_offset = addr_to_offset(max_addr) - page_offset;
        for (uint32_t index = 0; index <= max_offset; ++index) {
            uint32_t offset = page_offset + index;
            for (uint32_t i = 0; i < MAX_THREADS; ++i) {
                uint32_t pos = workers[i]->get_addr_table(offset);
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
    printf("Counted %d addresses, tot_count: %d, tot_count2:%d\n", count, tot_count, tot_count2);

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
        uint32_t max_offset = addr_to_offset(max_addr) - page_offset;
        for (uint32_t index = 0; index <= max_offset; ++index) {
            uint32_t offset = page_offset + index;
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
