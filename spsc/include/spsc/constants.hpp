#pragma once
#include <cstddef>

namespace spsc {

// Apple Silicon uses 128-byte cache lines; x86-64 uses 64
#if defined(__APPLE__) && defined(__aarch64__)
inline constexpr std::size_t CACHE_LINE_SIZE = 128;
#else
inline constexpr std::size_t CACHE_LINE_SIZE = 64;
#endif

}  //  namespace spsc