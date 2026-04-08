#pragma once

#include <cstddef>

#if defined(_WIN32)
#include <thread>
#else
#include <unistd.h>
#endif

/** Portable guess for worker count (avoids std::thread on POSIX). */
inline std::size_t hardwareConcurrencyOrOne() {
#if defined(_WIN32)
  unsigned c = std::thread::hardware_concurrency();
  return c == 0 ? 1 : static_cast<std::size_t>(c);
#else
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? static_cast<std::size_t>(n) : 1;
#endif
}
