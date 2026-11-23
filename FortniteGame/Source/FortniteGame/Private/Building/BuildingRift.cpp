#include "pch.h"
#include "Building/BuildingRift.h"
#include "Finder.h"
#include "Hooks.h"
#include "Log.h"
#include "Logging/LogMacros.h"
#include "Missions/FortCreativeRiftSpawnParams.h"
#include "Missions/FortMissionLibrary.h"
#include "ReflectionParams.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

#include <algorithm>

DEFINE_LOG_CATEGORY_STATIC(LogFortBuilding, Log, All);

void ABuildingRift::BlueprintHideRift()
{
    UFunction* NativeEvent = FindStaticFunction_BlueprintHideRift();
    Reflection::FParams Params(NativeEvent ? FindFunction(NativeEvent->GetFName()) : nullptr);
    Params.Call(this);
}

void AAthenaCreativeRift::BeginPlay()
{
    BeginPlayOG(this);

#if WITH_SERVER_CODE
    // 8.20's BeginPlay (0xC41960; 8.00's the same) calls Super::BeginPlay and, when the game state has a mission manager,
    // makes the "RiftCreated" event with a UFortCreativeRiftSpawnParams of the rift and its OverrideEncounterSettings,
    // then destroys it: the HandleMissionEvent call between is compiled out of the client builds. ABuildingRift's own
    // RiftCreated (0xFC5EF0, the BR fragments' UFortRiftCreatedParams) and the creative rift's player-overlap event
    // (0xC52400) still make that call, through the game state's MissionManager. That send is what goes here, by the
    // game's UFortMissionLibrary::ExecuteMissionEvent, which builds the same event and hands it to the same manager.
    // Creative's mission (Mutator_CreativeMissions' Creative_Mission_RiftSpawners_Deimos, an AFortMission_RiftSpawners)
    // takes the rift into its spawners (8.20 0x1363990 -> 0x1356610) with a pointer to the settings, which it copies
    // into the encounter a player starts in the rift's overlap sphere.
    if (Role != ROLE_Authority)
    {
        return;
    }
    const FName RiftCreated(L"RiftCreated", FNAME_Find);
    UObject* TransientPackage = StaticFindObject(nullptr, nullptr, L"/Engine/Transient");
    UFortCreativeRiftSpawnParams* Params = NewObject<UFortCreativeRiftSpawnParams>(TransientPackage, UFortCreativeRiftSpawnParams::StaticClass());
    if (!Params || !Params->SetParams(this, &static_cast<FFortEncounterSettings&>(OverrideEncounterSettings)))
    {
        UE_LOG(LogFortBuilding, Error, TEXT("AAthenaCreativeRift::BeginPlay: %s couldn't make its RiftCreated params"), *GetName());
        return;
    }
    UE_LOG(LogFortBuilding, Log, TEXT("AAthenaCreativeRift::BeginPlay: %s sends RiftCreated"), *GetName());
    UFortMissionLibrary::ExecuteMissionEvent(this, RiftCreated, Params);
#endif
}

void AAthenaCreativeRift::BeginPlayHK(AAthenaCreativeRift* This)
{
    This->BeginPlay();
}

void AAthenaCreativeRift::HookVTables()
{
#if WITH_SERVER_CODE
    // 7.40 adds the creative rift, but reflects neither its settings nor bHasLoadedSettings, which 8.00 does.
    UClass* Class = StaticClass();
    if (!Class)
    {
        return;
    }
    if (!HasEncounterSettings() || !UFortCreativeRiftSpawnParams::StaticClass() || !UFortMissionLibrary::FindStaticFunction_ExecuteMissionEvent()
        || FName(L"RiftCreated", FNAME_Find).IsNone())
    {
        Log::Info(L"Hook AAthenaCreativeRift::BeginPlay: not restored, this build doesn't reflect the creative rift's encounter settings");
        return;
    }

    // BeginPlay is the creative rift's override of an AActor slot that takes OverrideEncounterSettings' address (for the
    // event's params) and doesn't read bHasLoadedSettings: its overlap override (0xC52400) does both.
    UObject* Default = Class->GetDefaultObject(false);
    UObject* RiftDefault = ABuildingRift::StaticClass() ? ABuildingRift::StaticClass()->GetDefaultObject(false) : nullptr;
    UObject* ActorDefault = AActor::StaticClass()->GetDefaultObject(false);
    void** VTable = Default ? *reinterpret_cast<void***>(Default) : nullptr;
    void** RiftVTable = RiftDefault ? *reinterpret_cast<void***>(RiftDefault) : nullptr;
    void** ActorVTable = ActorDefault ? *reinterpret_cast<void***>(ActorDefault) : nullptr;
    const auto IsCode = [](const void* Address)
    {
        const auto& Sections = Finder::ExecutableSections();
        return std::any_of(Sections.begin(), Sections.end(), [Address](const auto& Section)
        {
            return reinterpret_cast<uintptr_t>(Address) >= Section.first && reinterpret_cast<uintptr_t>(Address) < Section.second;
        });
    };
    const int32_t SettingsOffset = Member_OverrideEncounterSettings.GetOffsetChecked();
    const int32_t LoadedOffset = Member_bHasLoadedSettings.GetOffsetChecked();
    int32_t BeginPlayIndex = -1;
    int32_t Matches = 0;
    for (int32_t Index = 1; VTable && RiftVTable && ActorVTable && Index < 0x200 && IsCode(ActorVTable[Index]); ++Index)
    {
        const uintptr_t Entry = reinterpret_cast<uintptr_t>(VTable[Index]);
        if (VTable[Index] != RiftVTable[Index] && IsCode(VTable[Index]) && Finder::TakesMemberAddressAt(Entry, SettingsOffset) && !Finder::ReadsMemberAt(Entry, LoadedOffset))
        {
            BeginPlayIndex = Index;
            ++Matches;
        }
    }
    if (Matches != 1)
    {
        Log::Error(L"Finder: AAthenaCreativeRift::BeginPlay not found ({} candidates; OverrideEncounterSettings +0x{:X}, bHasLoadedSettings +0x{:X}), so no creative rift reaches its mission",
            Matches, SettingsOffset, LoadedOffset);
        return;
    }

    void* Original = VTable[BeginPlayIndex];
    BeginPlayOG = reinterpret_cast<decltype(BeginPlayOG)>(Original);
    int32_t Swapped = 0;
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UObject* ClassDefault = It->IsChildOf(Class) ? It->GetDefaultObject(false) : nullptr;
        void** ClassVTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (ClassVTable && ClassVTable[BeginPlayIndex] == Original)
        {
            Hooks::SwapVTable(L"AAthenaCreativeRift::BeginPlay", ClassVTable, BeginPlayIndex, reinterpret_cast<void*>(&BeginPlayHK));
            ++Swapped;
        }
    }
    Log::Info(L"Finder: AAthenaCreativeRift::BeginPlay (slot 0x{:X}, 0x{:X}) builds RiftCreated without sending it, the send restored in {} classes",
        BeginPlayIndex, reinterpret_cast<uintptr_t>(Original) - Finder::ImageBase(), Swapped);
#endif
}
