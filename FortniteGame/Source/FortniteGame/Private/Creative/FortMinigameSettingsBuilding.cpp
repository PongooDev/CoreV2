#include "pch.h"
#include "Creative/FortMinigameSettingsBuilding.h"
#include "Containers/Set.h"
#include "Finder.h"
#include "Hooks.h"
#include "Log.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

void AFortMinigameSettingsBuilding::HookVTables()
{
#if WITH_SERVER_CODE
    // CoreV2 #684. 9.00 adds the replicated SettingsVolume: its OnRep (9.00 0x19922C0) is what links the machine to its
    // volume on a client (UFortMinigameVolumeComponent::CurrentMinigameSettingsMachine, which the GAME tab reads in
    // UFortMatchmakingKnobsModal::SetInScrimmage 0x3653790). The machine's own link can't: BeginPlay (0x1982500) binds
    // OnLinkedVolumeSet only after Super::BeginPlay, where the link component's BeginPlay (0x133D230 -> 0x1345970 ->
    // SetLinkedVolume 0x135FB90, its only caller) has already found the volume and broadcast. Nothing in the build
    // writes SettingsVolume (+0x2D8): its setter is server code, compiled out. The volume is the one the link component
    // found at BeginPlay; SetLinkedVolume has no later caller in the build.
    UClass* const Class = StaticClass();
    if (!Class || !Member_SettingsVolume.IsAvailable() || !Member_CreativeLinkComponent.IsAvailable() || !UFortCreativeVolumeLinkComponent::HasLinkedVolume()
        || !FindStaticFunction_OnRep_SettingsVolume())
    {
        Log::Info(L"Hook AFortMinigameSettingsBuilding::BeginPlay: not needed, this build's machine has no SettingsVolume");
        return;
    }
    const std::vector<uintptr_t> Refs = Finder::StringRefs(L"AFortMinigameSettingsBuilding::BeginPlay: FullName[%s]");
    const uintptr_t BeginPlay = Refs.empty() ? 0 : Finder::FunctionStart(Refs.front());
    UObject* const Default = Class->GetDefaultObject(false);
    void** const VTable = Default ? *reinterpret_cast<void***>(Default) : nullptr;
    int32_t BeginPlayIndex = -1;
    for (int32_t Index = 1; BeginPlay && VTable && Index < 0x200; ++Index)
    {
        if (reinterpret_cast<uintptr_t>(VTable[Index]) == BeginPlay)
        {
            BeginPlayIndex = Index;
            break;
        }
    }
    if (BeginPlayIndex < 0)
    {
        Log::Error(L"Finder: AFortMinigameSettingsBuilding::BeginPlay not found ({} refs to its log line, body 0x{:X}), so no island's GAME tab gets its machine",
            Refs.size(), BeginPlay ? BeginPlay - Finder::ImageBase() : 0);
        return;
    }

    void* const Original = VTable[BeginPlayIndex];
    BeginPlayOG = reinterpret_cast<decltype(BeginPlayOG)>(Original);
    TSet<void**> Swapped;
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UObject* const ClassDefault = It->IsChildOf(Class) ? It->GetDefaultObject(false) : nullptr;
        void** const ClassVTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (ClassVTable && !Swapped.Contains(ClassVTable) && ClassVTable[BeginPlayIndex] == Original)
        {
            Hooks::SwapVTable(L"AFortMinigameSettingsBuilding::BeginPlay", ClassVTable, BeginPlayIndex, reinterpret_cast<void*>(&BeginPlayHK));
            Swapped.Add(ClassVTable);
        }
    }
    Log::Info(L"Finder: AFortMinigameSettingsBuilding::BeginPlay (slot 0x{:X}, 0x{:X}) sets no SettingsVolume, restored in {} vtables",
        BeginPlayIndex, BeginPlay - Finder::ImageBase(), Swapped.Num());
#endif
}

void AFortMinigameSettingsBuilding::BeginPlayHK(AFortMinigameSettingsBuilding* This)
{
    BeginPlayOG(This);
#if WITH_SERVER_CODE
    UFortCreativeVolumeLinkComponent* const Link = This->CreativeLinkComponent;
    AFortVolume* const Volume = Link ? static_cast<AFortVolume*>(Link->LinkedVolume) : nullptr;
    if (!Volume || This->SettingsVolume == Volume)
    {
        return;
    }
    // For the clients only: their OnRep also binds the minigame's start/end to the volume's state (0x1991590), which the
    // server's own minigame flow sets.
    This->SettingsVolume = Volume;
    This->ForceNetUpdate();
#endif
}
