#include "object.h"
#include "native_jit_mem.h"

void NativeJitRegionDeleter::operator()(void* region) const {
    if (region != nullptr) {
        jit_free_executable(region, 0);
    }
}
