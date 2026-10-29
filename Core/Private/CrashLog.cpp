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
    thread_local bool bInHandler = false;

    bool IsCrash(DWORD Code)
    {
        switch (Code)
        {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_IN_PAGE_ERROR:
            return true;
        default:
            return false;
        }
    }

    std::wstring Describe(DWORD64 Address)
    {
        HMODULE Module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(Address), &Module) || !Module)
        {
            return std::format(L"0x{:X}", Address);
        }
        wchar_t Path[MAX_PATH] = {};
        GetModuleFileNameW(Module, Path, MAX_PATH);
        const std::wstring_view Name = Path;
        const size_t Slash = Name.find_last_of(L"\\/");
        return std::format(L"{}+0x{:X}", Slash == std::wstring_view::npos ? Name : Name.substr(Slash + 1), Address - reinterpret_cast<DWORD64>(Module));
    }

    LONG CALLBACK OnException(PEXCEPTION_POINTERS Pointers)
    {
        const EXCEPTION_RECORD* Record = Pointers ? Pointers->ExceptionRecord : nullptr;
        if (!Record || !Pointers->ContextRecord || !IsCrash(Record->ExceptionCode) || bInHandler || LoggedExceptions.fetch_add(1) >= MaxLoggedExceptions)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        bInHandler = true;

        // Logged when raised, before any handler: one the game catches and survives is logged too, hence "exception",
        // and only the first few are.
        std::wstring Text = std::format(L"Exception 0x{:08X} at {} (thread {})", Record->ExceptionCode, Describe(reinterpret_cast<DWORD64>(Record->ExceptionAddress)), GetCurrentThreadId());
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

        // A packed exe's (1.9.1) moved code has no unwind data and jumps through the stack, so the walk above can stop
        // at its first frame: the module addresses near the top of the stack show where the fault came from.
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
    if (!AddVectoredExceptionHandler(1, &OnException))
    {
        Log::Warning(L"CrashLog: AddVectoredExceptionHandler failed ({}), crashes won't be logged", GetLastError());
    }
}
