#ifndef uranium_native_jit_mem_h
#define uranium_native_jit_mem_h

#include <cstddef>

// Allocate raw virtual memory that can be marked executable.
void* jit_alloc_executable(std::size_t size);

// Free allocated virtual memory.
void jit_free_executable(void* ptr, std::size_t size);

// Mark the memory region as executable (used after writing code on hardened platforms like macOS ARM64).
void jit_make_executable(void* ptr, std::size_t size);

#endif // uranium_native_jit_mem_h
