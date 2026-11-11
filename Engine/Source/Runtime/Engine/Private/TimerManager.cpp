#include "pch.h"
#include "TimerManager.h"
#include "Engine/World.h"
#include "Finder.h"
#include "GameThread.h"
#include "Log.h"
#include "Natives.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <new>
#include <optional>
#include <vector>

namespace
{
    uint8_t Byte(uintptr_t Address)
    {
        return *reinterpret_cast<const uint8_t*>(Address);
    }

    // `mov r64, [r64+disp32]` at Site (REX.W 8B, ModRM mod 10, no SIB byte): the destination and base registers
    // (0-15) and the displacement.
    struct FLoad
    {
        int32_t Destination = -1;
        int32_t Base = -1;
        int32_t Displacement = 0;
    };

    std::optional<FLoad> LoadAt(uintptr_t Site)
    {
        const uint8_t Rex = Byte(Site);
        const uint8_t ModRm = Byte(Site + 2);
        if ((Rex & 0xF8) != 0x48 || (Rex & 0x02) != 0 || Byte(Site + 1) != 0x8B || (ModRm & 0xC0) != 0x80 || (ModRm & 0x07) == 0x04)
        {
            return std::nullopt;
        }
        FLoad Load;
        Load.Destination = ((ModRm >> 3) & 0x07) | ((Rex & 0x04) << 1);
        Load.Base = (ModRm & 0x07) | ((Rex & 0x01) << 3);
        Load.Displacement = *reinterpret_cast<const int32_t*>(Site + 3);
        return Load;
    }

    // Core's timers where the build's timer manager wasn't found: run from the game thread queue, by wall-clock time.
    struct FFallbackTimer
    {
        uint64_t Handle = 0;
        std::shared_ptr<FTimerDelegate> Delegate;
        std::chrono::steady_clock::time_point Due;
        float Rate = 0.0f;
        bool bLoop = false;
    };

    std::vector<FFallbackTimer> FallbackTimers;
    bool bFallbackQueued = false;
    uint64_t NextFallbackHandle = 1;

    std::chrono::steady_clock::duration Seconds(float Value)
    {
        return std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<float>(Value));
    }

    void RunFallbackTimers()
    {
        bFallbackQueued = false;
        const auto Now = std::chrono::steady_clock::now();
        std::vector<FFallbackTimer> Due;
        std::erase_if(FallbackTimers, [&](FFallbackTimer& Timer)
        {
            if (Timer.Due > Now)
            {
                return false;
            }
            Due.push_back(Timer);
            if (Timer.bLoop)
            {
                Timer.Due = Now + Seconds(Timer.Rate);
                return false;
            }
            return true;
        });
        // A delegate can set timers of its own: they go in after this round.
        for (const FFallbackTimer& Timer : Due)
        {
            Timer.Delegate->ExecuteIfBound();
        }
        if (!FallbackTimers.empty())
        {
            bFallbackQueued = GameThread::Enqueue(&RunFallbackTimers);
        }
    }
}

bool FTimerManager::ResolveNatives()
{
    static std::optional<bool> bResolved;
    if (bResolved)
    {
        return *bResolved;
    }
    bResolved = false;

    // UKismetSystemLibrary::K2_SetTimerDelegate (1.8.1 0x222C720, 5.0 0x2453C10, 5.30 0x255B380, 5.40 0x2594170) has
    // World->GetTimerManager() inlined, then TimerManager.K2_FindDynamicTimerHandle and SetTimer's InternalSetTimer:
    //   mov rcx, [rax+OwningGameInstance]; test rcx, rcx; jz; mov rsi, [rcx+TimerManager]; jmp; mov rsi, [rax+TimerManager]
    //   ... movaps xmm3, xmm6 (InRate); mov byte ptr [rsp+20h], r14b (InbLoop); call InternalSetTimer
    // 1.8.1's game instance has it at +0xD8 and the world at +0x400; 5.0-5.40's at +0x90 and +0x3F8.
    // The timer manager calls Core's delegate instances by the build's vtable slots: without them read from the build,
    // Core's timers stay on its own queue.
    if (!Natives::IDelegateInstance_GetHandleIndexResolved)
    {
        Log::Error(L"Finder: FTimerManager not used: the delegate instance layout wasn't read from the build; Core's timers run from its game thread queue");
        return false;
    }
    const std::vector<uintptr_t> Refs = Finder::StringRefs(L"SetTimer passed a bad function (%s) or object (%s)");
    const uintptr_t Function = Refs.empty() ? 0 : Finder::FunctionStart(Refs.front());
    const uintptr_t End = Function ? Finder::FunctionEnd(Function) : 0;
    const ptrdiff_t OwningGameInstance = End ? UWorld::OwningGameInstanceOffset() : -1;

    ptrdiff_t GameInstanceTimerManager = -1;
    ptrdiff_t WorldTimerManager = -1;
    uintptr_t AfterLoads = 0;
    for (uintptr_t Site = Function; OwningGameInstance > 0 && Site + 7 <= End && !AfterLoads; ++Site)
    {
        const std::optional<FLoad> GameInstance = LoadAt(Site);
        if (!GameInstance || GameInstance->Displacement != OwningGameInstance)
        {
            continue;
        }
        std::optional<FLoad> FromGameInstance;
        for (uintptr_t Next = Site + 7; Next + 7 <= Site + 0x30 && Next + 7 <= End; ++Next)
        {
            const std::optional<FLoad> Load = LoadAt(Next);
            if (!Load)
            {
                continue;
            }
            if (!FromGameInstance && Load->Base == GameInstance->Destination)
            {
                FromGameInstance = Load;
                Next += 6;
            }
            else if (FromGameInstance && Load->Base == GameInstance->Base && Load->Destination == FromGameInstance->Destination)
            {
                GameInstanceTimerManager = FromGameInstance->Displacement;
                WorldTimerManager = Load->Displacement;
                AfterLoads = Next + 7;
                break;
            }
        }
    }

    // The call after InRate goes into xmm3 (movaps xmm3, xmmN; 41 0F 28 for xmm8-15).
    uintptr_t InternalSetTimer = 0;
    for (uintptr_t Site = AfterLoads; Site && Site + 3 <= End && !InternalSetTimer; ++Site)
    {
        const uintptr_t Op = Byte(Site) == 0x41 ? Site + 1 : Site;
        if (Byte(Op) != 0x0F || Byte(Op + 1) != 0x28 || (Byte(Op + 2) & 0xF8) != 0xD8)
        {
            continue;
        }
        for (const auto& [Call, Target] : Finder::DirectCalls(Op + 3, std::min(Op + 0x60, End)))
        {
            if (Target >= Finder::TextBegin() && Target < Finder::TextEnd() && Finder::FunctionStart(Target) == Target)
            {
                InternalSetTimer = Target;
                break;
            }
        }
    }

    const auto Rva = [](uintptr_t Address) { return Address ? Address - Finder::ImageBase() : 0; };
    if (GameInstanceTimerManager <= 0 || WorldTimerManager <= 0 || !InternalSetTimer)
    {
        Log::Error(L"Finder: FTimerManager not found (K2_SetTimerDelegate 0x{:X}, game instance's +0x{:X}, world's +0x{:X}, InternalSetTimer 0x{:X}); Core's timers run from its game thread queue",
            Rva(Function), GameInstanceTimerManager, WorldTimerManager, Rva(InternalSetTimer));
        return false;
    }
    Natives::UGameInstance_TimerManager = GameInstanceTimerManager;
    Natives::UWorld_TimerManager = WorldTimerManager;
    Natives::FTimerManager_InternalSetTimer = reinterpret_cast<decltype(Natives::FTimerManager_InternalSetTimer)>(InternalSetTimer);
    Log::Info(L"Finder: FTimerManager::InternalSetTimer 0x{:X}, UGameInstance::TimerManager +0x{:X}, UWorld::TimerManager +0x{:X} (K2_SetTimerDelegate 0x{:X})",
        Rva(InternalSetTimer), GameInstanceTimerManager, WorldTimerManager, Rva(Function));
    bResolved = true;
    return true;
}

FTimerManager& FTimerManager::Fallback()
{
    static FTimerManager Manager;
    return Manager;
}

void FTimerManager::SetTimer(FTimerHandle& InOutHandle, FTimerDelegate const& InDelegate, float InRate, bool InbLoop, float InFirstDelay)
{
    if (this != &Fallback() && Natives::FTimerManager_InternalSetTimer)
    {
        // FTimerUnifiedDelegate(InDelegate): FuncDelegate, then FuncDynDelegate and FuncCallback (a TFunction), which
        // stay unbound. Zero is their unbound state in every build, and the whole is at most 0xA0 bytes (0x98 up to
        // 4.1, with the 0x40-byte inline delegate; 0x68 from 4.2). InternalSetTimer moves the delegate into the timer.
        alignas(16) uint8_t UnifiedDelegate[0x100] = {};
        FTimerDelegate* FuncDelegate = new (UnifiedDelegate) FTimerDelegate();
        FuncDelegate->CopyFrom(InDelegate);
        Natives::FTimerManager_InternalSetTimer(this, &InOutHandle, UnifiedDelegate, InRate, InbLoop, InFirstDelay);
        FuncDelegate->~FTimerDelegate();
        return;
    }

    std::erase_if(FallbackTimers, [&InOutHandle](const FFallbackTimer& Timer) { return InOutHandle.IsValid() && Timer.Handle == InOutHandle.Handle; });
    InOutHandle.Invalidate();
    if (InRate <= 0.0f)
    {
        return;
    }
    FFallbackTimer Timer;
    Timer.Handle = NextFallbackHandle++;
    Timer.Delegate = std::make_shared<FTimerDelegate>();
    Timer.Delegate->CopyFrom(InDelegate);
    Timer.Due = std::chrono::steady_clock::now() + Seconds(InFirstDelay >= 0.0f ? InFirstDelay : InRate);
    Timer.Rate = InRate;
    Timer.bLoop = InbLoop;
    InOutHandle.Handle = Timer.Handle;
    FallbackTimers.push_back(std::move(Timer));
    if (!bFallbackQueued)
    {
        bFallbackQueued = GameThread::Enqueue(&RunFallbackTimers);
    }
}
