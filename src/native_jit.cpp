#include "native_jit.h"

// Prototypes for architecture-specific JIT compilers
#if defined(_WIN32) && defined(_M_IX86)
bool compileNativeJit_x86(const FunctionPtr& function,
                          const FastPathPlan& plan,
                          NativeJitArtifact* artifact,
                          std::string* reason);
#endif

#if (defined(_WIN32) && defined(_M_X64)) || defined(__x86_64__)
bool compileNativeJit_x64(const FunctionPtr& function,
                          const FastPathPlan& plan,
                          NativeJitArtifact* artifact,
                          std::string* reason);
#endif

#if defined(_M_ARM) || defined(__arm__)
bool compileNativeJit_arm32(const FunctionPtr& function,
                            const FastPathPlan& plan,
                            NativeJitArtifact* artifact,
                            std::string* reason);
#endif

#if defined(_M_ARM64) || defined(__aarch64__)
bool compileNativeJit_arm64(const FunctionPtr& function,
                            const FastPathPlan& plan,
                            NativeJitArtifact* artifact,
                            std::string* reason);
#endif

bool compileNativeJit_wasm(const FunctionPtr& function,
                           const FastPathPlan& plan,
                           NativeJitArtifact* artifact,
                           std::string* reason);

bool compileNativeJit(const FunctionPtr& function,
                      const FastPathPlan& plan,
                      NativeJitArtifact* artifact,
                      std::string* reason,
                      bool targetWasm) {
    if (targetWasm) {
        return compileNativeJit_wasm(function, plan, artifact, reason);
    }

#if defined(_WIN32) && defined(_M_IX86)
    return compileNativeJit_x86(function, plan, artifact, reason);
#elif (defined(_WIN32) && defined(_M_X64)) || defined(__x86_64__)
    return compileNativeJit_x64(function, plan, artifact, reason);
#elif defined(_M_ARM) || defined(__arm__)
    return compileNativeJit_arm32(function, plan, artifact, reason);
#elif defined(_M_ARM64) || defined(__aarch64__)
    return compileNativeJit_arm64(function, plan, artifact, reason);
#else
    if (reason != nullptr) {
        *reason = "platform";
    }
    return false;
#endif
}
