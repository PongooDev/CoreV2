#pragma once

#include "Misc/Build.h"
#include "Natives.h"

#include <cstdint>

struct FDebug
{
    template <typename... TArgs>
    static void AssertFailed(const char* Expr, const char* File, int32_t Line, const wchar_t* Format = L"", TArgs... Args)
    {
        Natives::FDebug_AssertFailed(Expr, File, Line, Format, Args...);
    }
};

template <typename... TArgs>
void LowLevelFatalErrorHandler(const char* File, int32_t Line, const wchar_t* Format = L"", TArgs... Args)
{
    Natives::LowLevelFatalErrorHandler(File, Line, Format, Args...);
}

#if UE_BUILD_SHIPPING
    #define _DebugBreakAndPromptForRemote()
#else
    #define _DebugBreakAndPromptForRemote() { if (IsDebuggerPresent()) { __debugbreak(); } }
#endif

#if DO_CHECK
    #define checkCode(Code) do { Code; } while (false);
    #define verify(expr) { if (!(expr)) { _DebugBreakAndPromptForRemote(); FDebug::AssertFailed(#expr, __FILE__, __LINE__); } }
    #define check(expr) { if (!(expr)) { _DebugBreakAndPromptForRemote(); FDebug::AssertFailed(#expr, __FILE__, __LINE__); } }
    #define verifyf(expr, format, ...) { if (!(expr)) { _DebugBreakAndPromptForRemote(); FDebug::AssertFailed(#expr, __FILE__, __LINE__, format, ##__VA_ARGS__); } }
    #define checkf(expr, format, ...) { if (!(expr)) { _DebugBreakAndPromptForRemote(); FDebug::AssertFailed(#expr, __FILE__, __LINE__, format, ##__VA_ARGS__); } }
    #define checkNoEntry() { _DebugBreakAndPromptForRemote(); FDebug::AssertFailed("Enclosing block should never be called", __FILE__, __LINE__); }
    #define unimplemented() { _DebugBreakAndPromptForRemote(); FDebug::AssertFailed("Unimplemented function called", __FILE__, __LINE__); }
#else
    #define checkCode(...)
    #define check(expr) { __assume(expr); }
    #define checkf(expr, format, ...) { __assume(expr); }
    #define checkNoEntry()
    #define verify(expr) { if (!(expr)) { __assume(false); } }
    #define verifyf(expr, format, ...) { if (!(expr)) { __assume(false); } }
    #define unimplemented() { __assume(false); }
#endif

#if DO_GUARD_SLOW
    #define checkSlow(expr) check(expr)
    #define checkfSlow(expr, format, ...) checkf(expr, format, ##__VA_ARGS__)
    #define verifySlow(expr) verify(expr)
#else
    #define checkSlow(expr) { __assume(expr); }
    #define checkfSlow(expr, format, ...) { __assume(expr); }
    #define verifySlow(expr) { if (!(expr)) { __assume(false); } }
#endif

#define ensure(InExpression) (!!(InExpression))
#define ensureMsgf(InExpression, InFormat, ...) (!!(InExpression))
#define ensureAlways(InExpression) (!!(InExpression))
#define ensureAlwaysMsgf(InExpression, InFormat, ...) (!!(InExpression))

struct FTCharArrayTester
{
    template <uint32_t N>
    static char (&Func(const wchar_t (&)[N]))[2];
    static char (&Func(...))[1];
};

#define IS_TCHAR_ARRAY(expr) (sizeof(FTCharArrayTester::Func(expr)) == 2)

#define LowLevelFatalError(Format, ...) \
    { \
        static_assert(IS_TCHAR_ARRAY(Format), "Formatting string must be a TCHAR array."); \
        LowLevelFatalErrorHandler(__FILE__, __LINE__, Format, ##__VA_ARGS__); \
        _DebugBreakAndPromptForRemote(); \
        FDebug::AssertFailed("", __FILE__, __LINE__, Format, ##__VA_ARGS__); \
    }
