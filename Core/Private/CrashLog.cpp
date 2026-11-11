#include "pch.h"
#include "CrashLog.h"
#include "Log.h"

#include <atomic>
#include <string>

namespace
{
    // A shipping build's own crash report ("=== Critical error: ===") has no symbols, and its call stack came out as
    // words of the stack instead (CoreV2 #113), so a crash said nowhere where it happened. Core logs the exception
    // itself: the faulting instruction and the return addresses unwound from its context, each as module+offset (an
    // IDB's address when the IDB's image base is 0), before the engine's handler runs.
    constexpr int MaxLoggedExceptions = 4;
    constexpr int MaxFrames = 48;

    std::atomic<int> LoggedExceptions = 0;
    std::atomic<int> LoggedOtherExceptions = 0;
    thread_local bool bInHandler = false;

    // The engine's fatal errors (UE_LOG Fatal, a failed check, LowLevelFatalError, Core's own check) end in
    // FPlatformMisc::RaiseException(1) (1.8.1 0x135B6C0, from FOutputDeviceWindowsError::Serialize and
    // FDebug::AssertFailed). A 1.8.1 server died with none of the codes below and nothing in its Core log (CoreV2 #352).
    constexpr DWORD EngineFatalErrorCode = 1;
    // MSVC's C++ throw. Core catches none, so one Core throws ends the process.
    constexpr DWORD CppExceptionCode = 0xE06D7363;

    [[maybe_unused]] bool IsThrownByCore(const EXCEPTION_RECORD* Record)
    {
        // ExceptionInformation[3] is the image base of the module that threw (x64).
        static const ULONG_PTR CoreBase = []
        {
            HMODULE Module = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&IsThrownByCore), &Module);
            return reinterpret_cast<ULONG_PTR>(Module);
        }();
        return Record->NumberParameters >= 4 && CoreBase && Record->ExceptionInformation[3] == CoreBase;
    }

    bool IsCrash(const EXCEPTION_RECORD* Record)
    {
        switch (Record->ExceptionCode)
        {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_IN_PAGE_ERROR:
            return true;
#if WITH_SERVER_CODE
        case EXCEPTION_STACK_OVERFLOW:
        case EngineFatalErrorCode:
            return true;
        case CppExceptionCode:
            return IsThrownByCore(Record);
#endif
        default:
            return false;
        }
    }

    // Any other exception of error severity (0xC0000000 and up, other modules' C++ throws among them). The engine's crash
    // handler takes every code: the __except around GuardedMain and each FRunnableThreadWin::Run (ReportCrash, which
    // starts CrashReportClient.exe) goes on to HandleError and FPlatformMisc::RequestExit(true), whose TerminateProcess
    // gives exit code 3 (GIsCriticalError ? 3 : 0, 1.8.1 0x135D050). A 1.8.1 server exited with 3 (CoreV2 #352), so a
    // crash on a code outside IsCrash's list is named too. These have their own count, so ones the game survives can't
    // use up the crashes' lines.
    [[maybe_unused]] bool IsOtherError(const EXCEPTION_RECORD* Record)
    {
        return (Record->ExceptionCode & 0xC0000000) == 0xC0000000;
    }

    std::wstring ModuleName(HMODULE Module)
    {
        wchar_t Path[MAX_PATH] = {};
        if (!GetModuleFileNameW(Module, Path, MAX_PATH))
        {
            return std::format(L"0x{:X}", reinterpret_cast<uintptr_t>(Module));
        }
        const std::wstring_view Name = Path;
        const size_t Slash = Name.find_last_of(L"\\/");
        return std::wstring(Slash == std::wstring_view::npos ? Name : Name.substr(Slash + 1));
    }

    std::wstring Describe(DWORD64 Address)
    {
        HMODULE Module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(Address), &Module) || !Module)
        {
            return std::format(L"0x{:X}", Address);
        }
        return std::format(L"{}+0x{:X}", ModuleName(Module), Address - reinterpret_cast<DWORD64>(Module));
    }

#if WITH_SERVER_CODE
    // A C++ throw's type (its ThrowInfo's first catchable type, decorated, e.g. ".?AVout_of_range@std@@") and the module
    // that threw it. ExceptionInformation: [0] the MSVC magic, [2] the ThrowInfo, [3] the thrower's image base, which the
    // x64 ThrowInfo's offsets are relative to: ThrowInfo { attributes, pmfnUnwind, pForwardCompat, pCatchableTypeArray },
    // CatchableTypeArray { count, types[] }, CatchableType { properties, pType, ... }, TypeDescriptor { vftable, spare,
    // name[] }.
    std::wstring DescribeCppException(const EXCEPTION_RECORD* Record)
    {
        if (Record->NumberParameters < 4 || Record->ExceptionInformation[0] < 0x19930520 || Record->ExceptionInformation[0] > 0x19930522)
        {
            return L" (C++ exception)";
        }
        const auto ImageBase = static_cast<uintptr_t>(Record->ExceptionInformation[3]);
        const auto* ThrowInfo = reinterpret_cast<const int32_t*>(Record->ExceptionInformation[2]);
        std::wstring Type = L"of unknown type";
        if (ImageBase && ThrowInfo && !IsBadReadPtr(ThrowInfo, 4 * sizeof(int32_t)) && ThrowInfo[3])
        {
            const auto* Types = reinterpret_cast<const int32_t*>(ImageBase + ThrowInfo[3]);
            const auto* Catchable = !IsBadReadPtr(Types, 2 * sizeof(int32_t)) && Types[0] > 0 ? reinterpret_cast<const int32_t*>(ImageBase + Types[1]) : nullptr;
            const char* Name = Catchable && !IsBadReadPtr(Catchable, 2 * sizeof(int32_t)) ? reinterpret_cast<const char*>(ImageBase + Catchable[1] + 2 * sizeof(void*)) : nullptr;
            if (Name && !IsBadStringPtrA(Name, 256))
            {
                Type.assign(Name, Name + strnlen(Name, 256));
            }
        }
        return std::format(L" (C++ exception {} thrown by {})", Type, ImageBase ? ModuleName(reinterpret_cast<HMODULE>(ImageBase)) : L"an unknown module");
    }
#endif

    std::wstring KindOf(const EXCEPTION_RECORD* Record)
    {
        switch (Record->ExceptionCode)
        {
        case EXCEPTION_STACK_OVERFLOW:
            return L" (stack overflow)";
        case EngineFatalErrorCode:
            return L" (engine fatal error, the reason is in FortniteGame.log)";
#if WITH_SERVER_CODE
        case CppExceptionCode:
            return DescribeCppException(Record);
#endif
        default:
            return L"";
        }
    }

    LONG CALLBACK OnException(PEXCEPTION_POINTERS Pointers)
    {
        const EXCEPTION_RECORD* Record = Pointers ? Pointers->ExceptionRecord : nullptr;
        if (!Record || !Pointers->ContextRecord || bInHandler)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        const bool bCrash = IsCrash(Record);
#if WITH_SERVER_CODE
        const bool bOtherError = !bCrash && IsOtherError(Record);
#else
        const bool bOtherError = false;
#endif
        if (bCrash ? LoggedExceptions.fetch_add(1) >= MaxLoggedExceptions : !bOtherError || LoggedOtherExceptions.fetch_add(1) >= MaxLoggedExceptions)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        bInHandler = true;

        // Logged when raised, before any handler: one the game catches and survives is logged too, hence "exception",
        // and only the first few are.
        std::wstring Text = std::format(L"Exception 0x{:08X}{} at {} (thread {})", Record->ExceptionCode, KindOf(Record), Describe(reinterpret_cast<DWORD64>(Record->ExceptionAddress)), GetCurrentThreadId());
        if ((Record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || Record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) && Record->NumberParameters >= 2)
        {
            const ULONG_PTR Operation = Record->ExceptionInformation[0];
            Text += std::format(L", {} 0x{:X}", Operation == 0 ? L"reading" : Operation == 8 ? L"executing" : L"writing", Record->ExceptionInformation[1]);
            MEMORY_BASIC_INFORMATION Page = {};
            if (VirtualQuery(reinterpret_cast<const void*>(Record->ExceptionInformation[1]), &Page, sizeof(Page)))
            {
                Text += std::format(L" (page protection 0x{:X}, state 0x{:X})", Page.Protect, Page.State);
            }
        }

        // The stack from the faulting frame, unwound with the modules' unwind data (RtlVirtualUnwind); a frame without
        // any (a leaf, or code without .pdata) returns to the address at the top of its stack.
        CONTEXT Context = *Pointers->ContextRecord;
        for (int Frame = 0; Frame < MaxFrames && Context.Rip; ++Frame)
        {
            Text += std::format(L"\n    #{:02} {}", Frame, Describe(Context.Rip));
            DWORD64 ImageBase = 0;
            PRUNTIME_FUNCTION Function = RtlLookupFunctionEntry(Context.Rip, &ImageBase, nullptr);
            if (Function)
            {
                void* HandlerData = nullptr;
                DWORD64 EstablisherFrame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, ImageBase, Context.Rip, Function, &Context, &HandlerData, &EstablisherFrame, nullptr);
            }
            else
            {
                if (IsBadReadPtr(reinterpret_cast<const void*>(Context.Rsp), sizeof(DWORD64)))
                {
                    break;
                }
                Context.Rip = *reinterpret_cast<const DWORD64*>(Context.Rsp);
                Context.Rsp += sizeof(DWORD64);
            }
        }

        // 1.9.1's moved code (Finder::HasMovedCode) has no unwind data and jumps through the stack, so the walk above
        // can stop at its first frame: the module addresses near the top of the stack show where the fault came from.
        const CONTEXT& Fault = *Pointers->ContextRecord;
        Text += std::format(L"\n    rax {:X} rcx {:X} rdx {:X} r8 {:X} r9 {:X} rsp {:X}", Fault.Rax, Fault.Rcx, Fault.Rdx, Fault.R8, Fault.R9, Fault.Rsp);
        int32_t Listed = 0;
        for (int32_t Slot = 0; Slot < 96 && Listed < 12; ++Slot)
        {
            const auto* Value = reinterpret_cast<const DWORD64*>(Fault.Rsp) + Slot;
            if (IsBadReadPtr(Value, sizeof(DWORD64)))
            {
                break;
            }
            HMODULE Module = nullptr;
            if (*Value && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(*Value), &Module))
            {
                Text += std::format(L"\n    [rsp+{:X}] {}", Slot * 8, Describe(*Value));
                ++Listed;
            }
        }

        Log::Error(L"{}", Text);
        bInHandler = false;
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

void CrashLog::Install()
{
#if WITH_SERVER_CODE
    // On an unpacked build Install runs on the main (game) thread: a stack overflow there leaves the handler this much
    // stack to log it with.
    ULONG StackGuarantee = 64 * 1024;
    SetThreadStackGuarantee(&StackGuarantee);
#endif
    if (!AddVectoredExceptionHandler(1, &OnException))
    {
        Log::Warning(L"CrashLog: AddVectoredExceptionHandler failed ({}), crashes won't be logged", GetLastError());
    }
}
