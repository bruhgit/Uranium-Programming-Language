#include "native_jit.h"
#include "native_jit_mem.h"

#if defined(_WIN32) && defined(_M_IX86)

bool compileNativeJit_x86(const FunctionPtr& function,
                          const FastPathPlan& plan,
                          NativeJitArtifact* artifact,
                          std::string* reason) {
    (void)function;
    (void)plan;
    (void)artifact;
    if (reason) {
        *reason = "x86_32_not_implemented";
    }
    return false; // Stub for 32-bit x86 Intel processors
}

#endif
