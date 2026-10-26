#include "pch.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineLogs.h"
#include "Finder.h"

DEFINE_LOG_CATEGORY(LogLevel);

bool ULevel::HasVisibilityChangeRequestPending() const
{
    UWorld* World = OwningWorld;
    return World && (this == World->CurrentLevelPendingVisibility || this == World->CurrentLevelPendingInvisibility);
}

TArray<AActor*>& ULevel::GetActors() const
{
    // Actors is native. UWorld::SpawnActor's Level->Actors.Add is its first `mov [reg+Num], ecx; cmp ecx, [reg+Num+4]`
    // (ArrayNum, ArrayMax): +0xA0 in every build. It sits right before OwningWorld on 1.1 and 1.2, but 1.7.2 puts
    // ActorsForGC between them.
    static const ptrdiff_t Offset = []
    {
        const auto Byte = [](uintptr_t Address) { return *reinterpret_cast<const uint8_t*>(Address); };
        const std::vector<uintptr_t> Refs = Finder::StringRefs(L"SpawnActor failed because no class was specified");
        const uintptr_t Spawn = Refs.empty() ? 0 : Finder::FunctionStart(Refs.front());
        const uintptr_t End = Spawn ? Finder::FunctionEnd(Spawn) : 0;
        for (uintptr_t Site = Spawn; End && Site + 16 <= End; ++Site)
        {
            const uintptr_t Mov = Site + (Byte(Site) == 0x41 ? 1 : 0);
            if (Byte(Mov) != 0x89 || (Byte(Mov + 1) & 0xF8) != 0x88 || (Byte(Mov + 1) & 0x07) == 0x04)
            {
                continue;
            }
            const int32_t Num = *reinterpret_cast<const int32_t*>(Mov + 2);
            const uintptr_t Cmp = Mov + 6 + (Byte(Mov + 6) == 0x41 ? 1 : 0);
            if (Byte(Cmp) == 0x3B && Byte(Cmp + 1) == Byte(Mov + 1) && *reinterpret_cast<const int32_t*>(Cmp + 2) == Num + 4)
            {
                UE_LOG(LogLevel, Log, TEXT("ULevel::Actors +0x%X (UWorld::SpawnActor)"), Num - 8);
                return static_cast<ptrdiff_t>(Num) - 8;
            }
        }
        UE_LOG(LogLevel, Error, TEXT("ULevel::Actors not found in UWorld::SpawnActor, using OwningWorld - 0x10"));
        return static_cast<ptrdiff_t>(-1);
    }();
    const ptrdiff_t Actors = Offset > 0 ? Offset : Member_OwningWorld.GetOffsetChecked() - static_cast<ptrdiff_t>(sizeof(TArray<AActor*>));
    return *reinterpret_cast<TArray<AActor*>*>(reinterpret_cast<uintptr_t>(this) + Actors);
}
