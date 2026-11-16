#include "pch.h"
#include "AI/FortNavSystem.h"
#include "Finder.h"
#include "Log.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "ReflectionParams.h"

#include <vector>

void UFortNavSystem::OnNavDataRegistered(ANavigationData* NavData)
{
    Reflection::FParams Params(StaticFunction_OnNavDataRegistered());
    Params.Set(L"NavData", reinterpret_cast<UObject*>(NavData));
    Params.Call(this);
}

namespace
{
    // PrepareBoundsAndUnlock's slot in UFortNavSystem's vtable (6.31 0x65). The function that logs its bounds, taken only
    // when it calls ConditionalPopulateNavOctree itself (a body that is there, not a stub left with its string).
    int32_t PrepareBoundsAndUnlockIndex()
    {
        static const int32_t Index = []
        {
            const std::vector<uintptr_t> Refs = Finder::StringRefs(L"Auto generating navigation bounds: %s");
            const uintptr_t Function = Refs.size() == 1 ? Finder::FunctionStart(Refs[0]) : 0;
            const uintptr_t FunctionEnd = Function ? Finder::FunctionEnd(Function) : 0;
            const uintptr_t PopulateOctree = reinterpret_cast<uintptr_t>(Natives::UNavigationSystem_ConditionalPopulateNavOctree);
            bool bIntact = false;
            for (const auto& [Site, Target] : FunctionEnd && PopulateOctree ? Finder::DirectCalls(Function, FunctionEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>())
            {
                bIntact = bIntact || Target == PopulateOctree;
            }
            UObject* Default = bIntact && UFortNavSystem::StaticClass() ? UFortNavSystem::StaticClass()->GetDefaultObject() : nullptr;
            void* const* VTable = Default ? *reinterpret_cast<void* const* const*>(Default) : nullptr;
            for (int32_t Slot = 0; VTable && Slot < 0x200; ++Slot)
            {
                if (reinterpret_cast<uintptr_t>(VTable[Slot]) == Function)
                {
                    Log::Info(L"Finder: UFortNavSystem::PrepareBoundsAndUnlock is slot 0x{:X} (+0x{:X})", Slot, Function - Finder::ImageBase());
                    return Slot;
                }
            }
            Log::Info(L"Finder: UFortNavSystem::PrepareBoundsAndUnlock not found ({} log string refs, body {})", Refs.size(), bIntact ? L"intact" : L"not found or stripped");
            return -1;
        }();
        return Index;
    }
}

bool UFortNavSystem::PrepareBoundsAndUnlock(const FNavigationBounds& Bounds)
{
    const int32_t Index = PrepareBoundsAndUnlockIndex();
    if (Index < 0)
    {
        return false;
    }
    CallVirtual<void>(this, Index, &Bounds);
    return true;
}
