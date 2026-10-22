#include "pch.h"
#include "FortVisibilityComponent.h"
#include "Finder.h"
#include "FortVisibilityManager.h"
#include "Hooks.h"
#include "Log.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

#include <set>
#include <vector>

void UFortVisibilityComponent::BeginPlay()
{
    BeginPlayOG(this);

#if WITH_SERVER_CODE
    AActor* const ComponentOwner = GetOwner();
    AFortVisibilityManager* const VisibilityManager = ComponentOwner && ComponentOwner->Role == ROLE_Authority ? AFortVisibilityManager::Get(ComponentOwner) : nullptr;
    if (!VisibilityManager)
    {
        return;
    }

    if (bRegisterWithVisibilityManager)
    {
        VisibilityManager->RegisterVisibilityComponent(this);
    }
    if (bObserver)
    {
        VisibilityManager->RegisterObserver(ComponentOwner);
    }
#endif
}

void UFortVisibilityComponent::EndPlay(EEndPlayReason::Type EndPlayReason)
{
#if WITH_SERVER_CODE
    AActor* const ComponentOwner = GetOwner();
    if (AFortVisibilityManager* const VisibilityManager = ComponentOwner && ComponentOwner->Role == ROLE_Authority ? AFortVisibilityManager::Get(ComponentOwner) : nullptr)
    {
        if (bRegisterWithVisibilityManager)
        {
            VisibilityManager->UnregisterVisibilityComponent(this);
        }
        if (bObserver)
        {
            VisibilityManager->UnregisterObserver(ComponentOwner);
        }
    }
#endif

    EndPlayOG(this, EndPlayReason);
}

void UFortVisibilityComponent::HookVTables()
{
    UObject* const ComponentDefault = UActorComponent::StaticClass()->GetDefaultObject(false);
    UObject* const VisibilityDefault = StaticClass()->GetDefaultObject(false);
    void** const ComponentVTable = ComponentDefault ? *reinterpret_cast<void***>(ComponentDefault) : nullptr;
    void** const VisibilityVTable = VisibilityDefault ? *reinterpret_cast<void***>(VisibilityDefault) : nullptr;

    std::vector<int32_t> SuperOnlySlots;
    for (int32_t Index = 0; ComponentVTable && VisibilityVTable && Index < 0x400; ++Index)
    {
        const uintptr_t Entry = reinterpret_cast<uintptr_t>(VisibilityVTable[Index]);
        const uintptr_t Base = reinterpret_cast<uintptr_t>(ComponentVTable[Index]);
        if (Entry < Finder::TextBegin() || Entry >= Finder::TextEnd() || Base < Finder::TextBegin() || Base >= Finder::TextEnd())
        {
            break;
        }
        if (Entry != Base && *reinterpret_cast<const uint8_t*>(Entry) == 0xE9 && Finder::RelativeTarget(Entry, 1, 5) == Base)
        {
            SuperOnlySlots.push_back(Index);
        }
    }

    if (SuperOnlySlots.size() != 2)
    {
        Log::Error(L"Finder: UFortVisibilityComponent BeginPlay/EndPlay slots not found ({} candidates)", SuperOnlySlots.size());
        return;
    }

    const struct
    {
        int32_t Index;
        void* Implementation;
        void** Original;
        const wchar_t* Name;
    } Restores[] = {
        { SuperOnlySlots[0], reinterpret_cast<void*>(&BeginPlayHK), reinterpret_cast<void**>(&BeginPlayOG), L"UFortVisibilityComponent::BeginPlay" },
        { SuperOnlySlots[1], reinterpret_cast<void*>(&EndPlayHK), reinterpret_cast<void**>(&EndPlayOG), L"UFortVisibilityComponent::EndPlay" },
    };

    for (const auto& Restore : Restores)
    {
        void* const Stripped = VisibilityVTable[Restore.Index];
        *Restore.Original = Stripped;

        std::set<uintptr_t> Overrides;
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Class = *It;
            UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
            void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
            if (!VTable)
            {
                continue;
            }
            if (VTable[Restore.Index] == Stripped)
            {
                Hooks::SwapVTable(Restore.Name, VTable, Restore.Index, Restore.Implementation);
            }
            else if (VTable[Restore.Index] != Restore.Implementation)
            {
                Overrides.insert(reinterpret_cast<uintptr_t>(VTable[Restore.Index]));
            }
        }

        for (const uintptr_t Override : Overrides)
        {
            const uintptr_t OverrideEnd = Finder::FunctionEnd(Override);
            for (const auto& [Site, Target] : OverrideEnd > Override ? Finder::DirectCalls(Override, OverrideEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>())
            {
                if (Target == reinterpret_cast<uintptr_t>(Stripped))
                {
                    Hooks::RedirectCall(Restore.Name, reinterpret_cast<uint8_t*>(Site), Restore.Implementation);
                }
            }
        }
    }
}

void UFortVisibilityComponent::BeginPlayHK(UFortVisibilityComponent* This)
{
    This->BeginPlay();
}

void UFortVisibilityComponent::EndPlayHK(UFortVisibilityComponent* This, EEndPlayReason::Type EndPlayReason)
{
    This->EndPlay(EndPlayReason);
}
