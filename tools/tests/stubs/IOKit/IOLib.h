#pragma once
#include <cstdlib>
#define IONewZero(T, count) static_cast<T *>(calloc(count, sizeof(T)))
#define IODelete(p, T, count) free(p)
inline void *IOMalloc(size_t size) { return malloc(size); }
inline void IOFree(void *p, size_t) { free(p); }
