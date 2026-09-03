#include "native_jit_mem.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#if defined(__APPLE__) && defined(__aarch64__)
#include <pthread.h>
extern "C" void pthread_jit_write_protect_np(int enabled);
#endif

void* jit_alloc_executable(std::size_t size) {
    std::size_t total_size = size + 16; // Add 16 bytes header to store size
#ifdef _WIN32
    void* raw_ptr = VirtualAlloc(nullptr, total_size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#else
    int flags = MAP_ANONYMOUS | MAP_PRIVATE;
#if defined(__APPLE__) && defined(__aarch64__)
    flags |= MAP_JIT;
#endif
    void* raw_ptr = mmap(nullptr, total_size, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (raw_ptr == MAP_FAILED) {
        return nullptr;
    }
#if defined(__APPLE__) && defined(__aarch64__)
    pthread_jit_write_protect_np(0); 
#endif
#endif

    if (raw_ptr == nullptr) return nullptr;
    
    // Write total allocated size to the first 8/16 bytes
    *static_cast<std::size_t*>(raw_ptr) = total_size;
    return static_cast<char*>(raw_ptr) + 16;
}

void jit_free_executable(void* ptr, std::size_t size) {
    (void)size;
    if (ptr == nullptr) return;
    void* raw_ptr = static_cast<char*>(ptr) - 16;
    std::size_t total_size = *static_cast<std::size_t*>(raw_ptr);
    
#ifdef _WIN32
    (void)total_size;
    VirtualFree(raw_ptr, 0, MEM_RELEASE);
#else
    munmap(raw_ptr, total_size);
#endif
}

void jit_make_executable(void* ptr, std::size_t size) {
    if (ptr == nullptr) return;
    void* raw_ptr = static_cast<char*>(ptr) - 16;
    std::size_t total_size = *static_cast<std::size_t*>(raw_ptr);
    
#ifdef _WIN32
    (void)size;
    (void)total_size;
    DWORD oldProtect;
    VirtualProtect(raw_ptr, total_size, PAGE_EXECUTE_READ, &oldProtect);
#elif defined(__APPLE__) && defined(__aarch64__)
    pthread_jit_write_protect_np(1);
    #if defined(__GNUC__) || defined(__clang__)
        __builtin___clear_cache(reinterpret_cast<char*>(ptr), reinterpret_cast<char*>(ptr) + size);
    #endif
#else
    mprotect(raw_ptr, total_size, PROT_READ | PROT_EXEC);
#endif
}
