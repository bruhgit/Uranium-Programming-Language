#include "object.h"

#ifdef _WIN32
#include <windows.h>
#endif

void NativeJitRegionDeleter::operator()(void* region) const {
#ifdef _WIN32
    if (region != nullptr) {
        VirtualFree(region, 0, MEM_RELEASE);
    }
#else
    (void)region;
#endif
}
