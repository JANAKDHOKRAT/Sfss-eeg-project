#ifndef SFSS_UTIL_HEADER
#define SFSS_UTIL_HEADER

#include <stdlib.h>
#include <cstdint> // for uint32_t
#include <iostream>
#include <ostream>
#include <stdexcept>
#include <cassert>
#include <cstring> // for memcpy

#include <vector>
#include <array>
#include <emp-tool/emp-tool.h>
#include <gmpxx.h>
#include "twokeyprp.h"

#include <thread>

using namespace emp;
typedef uint64_t index_type;

inline std::ostream &operator<<(std::ostream &os, __uint128_t value)
{
    if (value == 0)
    {
        os << "0";
        return os;
    }
    char buf[40] = {0};
    int i = 39;
    while (value > 0 && i)
    {
        buf[--i] = "0123456789"[value % 10];
        value /= 10;
    }
    os << (buf + i);
    return os;
}

// Helper for element size calculation
template <typename T>
size_t get_serialized_size_helper(const T &v) {
    if constexpr (std::is_arithmetic<T>::value) {
        return sizeof(T);
    } else {
        return v.get_serialized_size();
    }
}

// Helper for element serialization
template <typename T>
size_t serialize_helper(const T &v, char *buf) {
    if constexpr (std::is_arithmetic<T>::value) {
        std::memcpy(buf, &v, sizeof(T));
        return sizeof(T);
    } else {
        v.serialize(buf);
        return v.get_serialized_size();
    }
}

// Helper for element deserialization
template <typename T>
size_t deserialize_helper(T &v, const char *buf)
{
    if constexpr (std::is_arithmetic<T>::value) {
        std::memcpy(&v, buf, sizeof(T));
        return sizeof(T);
    } else {
        v.deserialize(buf);
        return v.get_serialized_size();
    }
}

#define my_assert(expr, msg)                              \
    if (!(expr))                                          \
    {                                                     \
        std::cerr << "Assertion failed: " << msg << "\n"; \
    }

// get the `i`-th bit of `idx`.
// NOTE: `N` <= 32.
template <int N = 32>
bool get_bit(index_type idx, uint64_t i)
{
    // i \in [1,2...,len]
    my_assert(N <= 64, "N should <= 64!");
    uint64_t mask = 1L << (N - i);
    uint64_t result = idx & mask;
    return (result == 0) ? false : true;
}

enum TimeitLevel { TIMEIT_NONE = 0, TIMEIT_LOG = 1, TIMEIT_WARNING = 2, TIMEIT_DEBUG = 3 };
extern TimeitLevel GLOBAL_TIMEIT_LEVEL;

TimeitLevel GLOBAL_TIMEIT_LEVEL = TIMEIT_DEBUG;

#define TIMEIT_START(name) auto start_##name = std::chrono::high_resolution_clock::now();
#define TIMEIT_END_LEVEL(name, level)                                                        \
    auto end_##name = std::chrono::high_resolution_clock::now();                             \
    if (level <= GLOBAL_TIMEIT_LEVEL) {                                                      \
        std::chrono::duration<double, std::milli> duration_##name = end_##name - start_##name; \
        std::cout << "Time taken for " << #name << ": "                                      \
                  << "\033[1;32m" << duration_##name.count() << " ms\033[0m\n" << std::endl; \
    }
#define TIMEIT_END(name) TIMEIT_END_LEVEL(name, TIMEIT_DEBUG)


#endif