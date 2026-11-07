#pragma once

// ============================================================================
// Windows platform configuration
// Every header that needs the Windows API includes this file first, so
// windows.h is always configured the same way.
// ============================================================================

// Disable the min/max macros from windows.h; they collide with std::min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif

// Trim what windows.h pulls in to speed up compilation.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

// Windows headers
#include <windows.h>

// Compiler-specific settings
#ifdef _MSC_VER
    // Silence specific warnings (adjust as the project needs).
    #pragma warning(push)
    #pragma warning(disable: 4505)  // unreferenced local function has been removed
#endif

// Debug logging macros
#ifdef _DEBUG
    #define DEBUG_LOG(msg) OutputDebugStringW(msg)
    #define DEBUG_LOGF(fmt, ...) { \
        wchar_t buf[512]; \
        swprintf_s(buf, fmt, __VA_ARGS__); \
        OutputDebugStringW(buf); \
    }
#else
    #define DEBUG_LOG(msg) ((void)0)
    #define DEBUG_LOGF(fmt, ...) ((void)0)
#endif
