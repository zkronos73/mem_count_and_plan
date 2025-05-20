#ifndef __SORTED_MEM_ADDRESS_COLLECTOR_HPP__
#define __SORTED_MEM_ADDRESS_COLLECTOR_HPP__

// SortedMemAddressCollector
#include <atomic>
#include <vector>
#include <algorithm>
#include <thread>
#include <mutex>
#include <condition_variable>

#include <immintrin.h>
#include <vector>

#include <atomic>
#include <vector>
#include "mem_config.hpp"
#include "circular_queue.hpp"

void insert_sorted(std::vector<uint32_t>& vec, uint32_t value) {
    // Encuentra la posición donde debería insertarse
    auto it = std::lower_bound(vec.begin(), vec.end(), value);
    // Inserta el elemento en la posición correcta
    vec.insert(it, value);
}



// Versión base con unrolling para procesar 2 chunks de 8 elementos por iteración
size_t avx2_lower_bound_unrolled(const std::vector<uint32_t>& data, uint32_t value) {
    const size_t n = data.size();
    if (n == 0) return 0;

    const __m256i key = _mm256_set1_epi32(value);
    size_t left = 0;
    size_t right = n;

    // Procesamiento de bloques grandes (32+ elementos)
    while (right - left >= 32) {
        size_t mid = left + (right - left) / 2;

        // Cargar dos chunks contiguos de 8 elementos cada uno
        __m256i chunk_lo = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(&data[mid]));
        __m256i chunk_hi = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(&data[mid + 8]));

        // Comparaciones vectorizadas
        __m256i cmp_lo = _mm256_cmpgt_epi32(key, chunk_lo);
        __m256i cmp_hi = _mm256_cmpgt_epi32(key, chunk_hi);

        // Convertir resultados a máscaras de bits
        int mask_lo = _mm256_movemask_epi8(cmp_lo);
        int mask_hi = _mm256_movemask_epi8(cmp_hi);

        // Toma de decisiones basada en las comparaciones
        if (mask_lo == 0xFFFFFFFF && mask_hi == 0xFFFFFFFF) {
            // Todos los elementos en ambos chunks son menores que el valor
            left = mid + 16;
        } else if (mask_lo != 0xFFFFFFFF) {
            // El valor está en el primer chunk o antes
            right = mid;
        } else {
            // El valor está en el segundo chunk
            // Búsqueda lineal dentro del segundo chunk (8 elementos)
            for (size_t i = 8; i < 16; ++i) {
                if (value <= data[mid + i]) {
                    return mid + i;
                }
            }
            left = mid + 16;
        }
    }

    // Procesar el resto con la versión normal (sin unrolling)
    // Versión auxiliar para el resto de elementos
    const uint32_t* arr = data.data();
    while (right - left >= 8) {
        size_t mid = left + (right - left) / 2;
        __m256i chunk = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(&arr[mid]));
        __m256i cmp = _mm256_cmpgt_epi32(key, chunk);
        int mask = _mm256_movemask_epi8(cmp);

        if (mask == 0xFFFFFFFF) {
            left = mid + 8;
        } else if (mask == 0) {
            right = mid;
        } else {
            // Búsqueda lineal en el chunk actual
            for (size_t i = 0; i < 8; ++i) {
                if (value <= arr[mid + i]) {
                    return mid + i;
                }
            }
            left = mid + 8;
        }
    }

    // Búsqueda lineal para los elementos restantes (<8)
    for (; left < right; ++left) {
        if (value <= arr[left]) {
            break;
        }
    }

    return left;
}

// Función de inserción optimizada
void avx2_insert_sorted(std::vector<uint32_t>& data, uint32_t value) {
    size_t pos = avx2_lower_bound_unrolled(data, value);
    data.insert(data.begin() + pos, value);
}


#define PRODUCERS_COUNT MAX_THREADS


typedef CircularQueue<uint32_t, 128> ProducerCircularQueue;

class SortedMemAddressCollector {
    bool producers_done = false;
    std::atomic<uint32_t> active_producers{0};
    ProducerCircularQueue producers[PRODUCERS_COUNT];
    std::vector<uint32_t> sorted_address;
public:
    SortedMemAddressCollector() {
    }
    ProducerCircularQueue *producer_start(uint32_t id) {
        sorted_address.reserve(4096);
        active_producers.fetch_add(1, std::memory_order_relaxed);
        return &producers[id];
    }

    void producer_finish(uint32_t id) {
        if (active_producers.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            producers_done = true;
        }
    }
    void producer_insert(uint32_t id, uint32_t value) {
        while (!producers[id].push(value)) {
            // Esperar a que haya espacio disponible
            std::this_thread::yield();
        }
    }
    void wait_for_producers() {
        bool any_processed = false;
        uint32_t addr;
        uint32_t counter;
        while (!producers_done) {
            do {
                any_processed = false;
                for (uint32_t i = 0; i < PRODUCERS_COUNT; ++i) {
                    if (!producers[i].pop(addr)) continue;

                    any_processed = true;
                    ++counter;
                    if (counter && 0xFFF == 0xEFF) {
                        sorted_address.reserve(sorted_address.capacity() + 4096);
                    }
                    sorted_address.push_back(addr);
                    //if ((counter & 0x0F) == 0) {
                    //    insert_sorted(sorted_address, addr);
                    //}
                    // avx2_insert_sorted(sorted_address, addr);
                }
            } while (any_processed);
            std::this_thread::yield();
        }
    }
    const std::vector<uint32_t> *get_sorted() {
        return &sorted_address;
    }
};

#endif