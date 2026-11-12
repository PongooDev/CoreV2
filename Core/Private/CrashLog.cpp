#include "pch.h"
#include "CrashLog.h"
#include "Log.h"

#if WITH_SERVER_CODE
#include "Engine/ActorChannel.h"
#include "Natives.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/Object.h"
#include "UObject/UObjectArray.h"
#endif

#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

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
    std::atomic<int> LoggedEngineErrorReports = 0;
    thread_local bool bInHandler = false;

    // The engine's fatal errors (UE_LOG Fatal, a failed check, LowLevelFatalError, Core's own check) end in
    // FPlatformMisc::RaiseException(1) (1.8.1 0x135B6C0, from FOutputDeviceWindowsError::Serialize and
    // FDebug::AssertFailed). A 1.8.1 server died with none of the codes below and nothing in its Core log (CoreV2 #352).
    // The ensures and FThreadHeartBeat's hang reports raise the same code inside their own __try, and the game carries
    // on: a 5.41 server's "Hang detected on GameThread" (a 56 s garbage collection on a PC short of memory, 2026-11-11)
    // went FThreadHeartBeat::Run (0x15C5760) -> the ensure report (0x16884B0) -> RaiseException(1) at 0x1689B20, and the
    // server kept running. So the line says it may be either, and these have their own count, so reports the game
    // survives can't use up the crashes' lines.
    constexpr DWORD EngineErrorReportCode = 1;
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
        case EngineErrorReportCode:
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
        case EngineErrorReportCode:
            return L" (engine error report: a fatal error, or an ensure or hang report the game carries on from; FortniteGame.log says which)";
#if WITH_SERVER_CODE
        case CppExceptionCode:
            return DescribeCppException(Record);
#endif
        default:
            return L"";
        }
    }

#if WITH_SERVER_CODE
    // The addresses say where a crash happened, not on what. A 6.02 server died in FObjectReplicator::ReplicateProperties
    // (0x250DF1B, CoreV2 #413), on an actor channel's replicator that had been freed: the report had the code's
    // addresses and five registers, and neither the channel (r12 there) nor its actor, so which actor it was, and
    // whether it was destroyed, could not be read from it. After the report, the registers it leaves out and the
    // objects that the registers and the faulting frames' stack point to are named: class, name, and whether the object
    // is pending kill. An actor channel's actor is named with it.
    constexpr int32_t MaxObjectStackSlots = 512;
    constexpr int32_t MaxListedObjects = 40;

    // Memory that may be anything is read without faulting.
    template <typename T>
    bool SafeRead(uintptr_t Address, T& Out)
    {
        SIZE_T Read = 0;
        return Address >= 0x10000 && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(Address), &Out, sizeof(T), &Read) && Read == sizeof(T);
    }

    // A live object's address is the one its InternalIndex's entry of GUObjectArray holds (UObjectBase: the vtable,
    // ObjectFlags, then InternalIndex at +0xC); a freed object's entry is empty or another object's.
    const FUObjectItem* ObjectItemAt(uintptr_t Value)
    {
        constexpr uintptr_t InternalIndexOffset = sizeof(void*) + sizeof(int32_t);
        const FUObjectArray* Array = Natives::GUObjectArray;
        int32_t Index = -1;
        if (!Array || (Value & 7) != 0 || !SafeRead(Value + InternalIndexOffset, Index) || Index < 0 || Index >= Array->GetObjectArrayNum())
        {
            return nullptr;
        }
        const FUObjectItem* Item = Array->IndexToObject(Index);
        uintptr_t Object = 0;
        return Item && SafeRead(reinterpret_cast<uintptr_t>(Item) + offsetof(FUObjectItem, Object), Object) && Object == Value ? Item : nullptr;
    }

    std::wstring DescribeObject(uintptr_t Value, const FUObjectItem* Item)
    {
        const UObject* Object = reinterpret_cast<const UObject*>(Value);
        const UClass* Class = Object->GetClass();
        const bool bClass = Class && ObjectItemAt(reinterpret_cast<uintptr_t>(Class));
        std::wstring Text = std::format(L"{} {}", bClass ? Class->GetName().ToStdString() : std::wstring(L"(no class)"), Object->GetName().ToStdString());
        if (Item->IsPendingKill())
        {
            Text += L" (pending kill)";
        }
        if (Item->IsUnreachable())
        {
            Text += L" (unreachable)";
        }
        return Text;
    }

    void LogObjectsUnguarded(const CONTEXT& Fault)
    {
        std::wstring Text = std::format(L"The exception's other registers, and the objects its registers and stack point to:\n    rbx {:X} rbp {:X} rsi {:X} rdi {:X} r10 {:X} r11 {:X} r12 {:X} r13 {:X} r14 {:X} r15 {:X}",
            Fault.Rbx, Fault.Rbp, Fault.Rsi, Fault.Rdi, Fault.R10, Fault.R11, Fault.R12, Fault.R13, Fault.R14, Fault.R15);
        std::vector<uintptr_t> Seen;
        int32_t Listed = 0;
        const auto List = [&Text, &Seen, &Listed](const std::wstring& Where, uintptr_t Value)
        {
            const FUObjectItem* Item = Listed < MaxListedObjects && std::find(Seen.begin(), Seen.end(), Value) == Seen.end() ? ObjectItemAt(Value) : nullptr;
            if (!Item)
            {
                return;
            }
            Seen.push_back(Value);
            ++Listed;
            Text += std::format(L"\n    {} {:X} is {}", Where, Value, DescribeObject(Value, Item));
            // The actor a channel replicates is what a crash in its replication is about.
            UActorChannel* Channel = Cast<UActorChannel>(reinterpret_cast<UObject*>(Value));
            const uintptr_t Actor = Channel ? reinterpret_cast<uintptr_t>(static_cast<AActor*>(Channel->Actor)) : 0;
            if (const FUObjectItem* ActorItem = Actor ? ObjectItemAt(Actor) : nullptr)
            {
                Text += std::format(L", whose actor {:X} is {}", Actor, DescribeObject(Actor, ActorItem));
            }
            else if (Channel)
            {
                Text += Actor ? std::format(L", whose actor {:X} is no live object", Actor) : std::wstring(L", which has no actor");
            }
        };
        const std::pair<const wchar_t*, DWORD64> Registers[] = {
            { L"rax", Fault.Rax }, { L"rbx", Fault.Rbx }, { L"rcx", Fault.Rcx }, { L"rdx", Fault.Rdx }, { L"rsi", Fault.Rsi }, { L"rdi", Fault.Rdi }, { L"rbp", Fault.Rbp },
            { L"r8", Fault.R8 }, { L"r9", Fault.R9 }, { L"r10", Fault.R10 }, { L"r11", Fault.R11 }, { L"r12", Fault.R12 }, { L"r13", Fault.R13 }, { L"r14", Fault.R14 }, { L"r15", Fault.R15 } };
        for (const auto& [Name, Value] : Registers)
        {
            List(Name, static_cast<uintptr_t>(Value));
        }
        for (int32_t Slot = 0; Slot < MaxObjectStackSlots; ++Slot)
        {
            uintptr_t Value = 0;
            if (!SafeRead(static_cast<uintptr_t>(Fault.Rsp) + Slot * sizeof(uintptr_t), Value))
            {
                break;
            }
            List(std::format(L"[rsp+{:X}]", Slot * sizeof(uintptr_t)), Value);
        }
        if (Listed == 0)
        {
            Text += L"\n    none of them is an object";
        }
        Log::Error(L"{}", Text);
    }

    // The names are the game's (FName::ToString, on the game's allocator), in a process that has just faulted: a second
    // fault in here ends this part only, the report itself is in the log by then.
    void LogObjects(const CONTEXT* Fault)
    {
        __try
        {
            LogObjectsUnguarded(*Fault);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }
#endif

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
        std::atomic<int>& Logged = Record->ExceptionCode == EngineErrorReportCode ? LoggedEngineErrorReports : bCrash ? LoggedExceptions : LoggedOtherExceptions;
        if ((!bCrash && !bOtherError) || Logged.fetch_add(1) >= MaxLoggedExceptions)
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
#if WITH_SERVER_CODE
        // A fault in code (not a stack overflow, which leaves no room for it, nor a report the code raised itself).
        if (bCrash && Record->ExceptionCode != EXCEPTION_STACK_OVERFLOW && Record->ExceptionCode != EngineErrorReportCode && Record->ExceptionCode != CppExceptionCode)
        {
            LogObjects(&Fault);
        }
#endif
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
