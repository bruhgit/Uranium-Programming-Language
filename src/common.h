#ifndef uranium_common_h
#define uranium_common_h

#include <cstdint>
#include <cstddef>
#include <iostream>

#if defined(URANIUM_DEBUG_TRACE)
#define DEBUG_TRACE_EXECUTION
#endif

#if defined(URANIUM_DEBUG_PRINT_CODE)
#define DEBUG_PRINT_CODE
#endif

#include <string>

extern std::size_t g_maxHeapBytes;
extern std::size_t g_baseYoungBytes;
extern std::size_t g_baseFullBytes;
extern int g_maxFrames;
extern bool g_vmDebugMode;

extern std::wstring g_compileIconPath;
extern std::wstring g_compileCompanyName;
extern std::wstring g_compileFileDescription;
extern std::wstring g_compileFileVersion;
extern std::wstring g_compileProductName;
extern std::wstring g_compileProductVersion;

extern int g_optimizerLevel;

#endif

