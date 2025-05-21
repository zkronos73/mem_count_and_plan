#ifndef __MEM_SEGMENT_HPP__
#define __MEM_SEGMENT_HPP__
#include <vector>
#include <unordered_map>
#include <stdexcept>
#include "mem_config.hpp"
#include "mem_check_point.hpp"

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
    MemSegment(uint32_t chunk_id, uint32_t from_addr, uint32_t skip, uint32_t count) : is_last_segment(false) {
        chunks.reserve(1024);
        chunks.try_emplace(chunk_id, std::move(MemCheckPoint(from_addr, skip, count)));
    }
    #ifdef MEM_CHECK_POINT_MAP
    void add_or_update(uint32_t chunk_id, uint32_t from_addr, uint32_t count) {
        auto result = chunks.try_emplace(chunk_id, std::move(MemCheckPoint(from_addr, 0, count)));
        if (!result.second) {
            result.first->second.add_rows(from_addr, count);
        }
    }
    #endif

};

#endif