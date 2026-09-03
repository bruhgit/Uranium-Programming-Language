#ifndef uranium_native_jit_h
#define uranium_native_jit_h

#include "object.h"
#include <string>

struct NativeJitArtifact {
    NativeJitRegionPtr region;
    void* entry;
    std::size_t size;

    NativeJitArtifact()
        : region(nullptr), entry(nullptr), size(0) {
    }
};

bool compileNativeJit(const FunctionPtr& function,
                      const FastPathPlan& plan,
                      NativeJitArtifact* artifact,
                      std::string* reason,
                      bool targetWasm = false);

#endif
