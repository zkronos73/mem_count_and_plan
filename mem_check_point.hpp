#ifndef __MEM_CHECK_POINT_HPP__
#define __MEM_CHECK_POINT_HPP__
#include <stdint.h>


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
    void add(uint32_t addr, uint32_t count) {
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

#endif