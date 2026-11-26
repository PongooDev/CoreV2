#include "pch.h"
#include "AbilitySystemComponent.h"
#include "AsyncTaskQueue.h"
#include "Athena/FortGameModeAthena.h"
#include "Athena/FortGameStateAthena.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "Athena/FortPlayerStateAthena.h"
#include "Athena/FortPlaylistAthena.h"
#include "Creative/FortAthenaCreativePortal.h"
#include "Creative/FortCreativeMoveTool.h"
#include "Creative/FortCreativePortalManager.h"
#include "Creative/FortLevelSaveComponent.h"
#include "Creative/FortVolume.h"
#include "Engine/World.h"
#include "Finder.h"
#include "FortGameInstance.h"
#include "FortGameModeZone.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerState.h"
#include "Hooks.h"
#include "FortKismetLibrary.h"
#include "Items/FortInventory.h"
#include "Items/FortItem.h"
#include "Items/FortItemDefinition.h"
#include "Kismet/GameplayStatics.h"
#include "Log.h"
#include "McpProfile.h"
#include "Minigame/FortMinigame.h"
#include "Natives.h"
#include "Online/FortMcpProfileCreative.h"
#include "Pawns/FortPlayerPawn.h"
#include "Player/FortPlayerState.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "TimerManager.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"
#include "Weapons/FortWeapon.h"

#include <algorithm>
#include <format>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace
{
    std::wstring PlayerNameOf(AFortPlayerControllerAthena* Controller)
    {
        APlayerState* PlayerState = Controller ? static_cast<APlayerState*>(reinterpret_cast<AController*>(Controller)->PlayerState) : nullptr;
        return PlayerState ? PlayerState->GetPlayerName().ToStdString() : std::wstring(L"a player");
    }

    const FUniqueNetIdRepl* UniqueIdOf(AFortPlayerControllerAthena* Controller)
    {
        APlayerState* PlayerState = Controller ? static_cast<APlayerState*>(reinterpret_cast<AController*>(Controller)->PlayerState) : nullptr;
        return PlayerState ? &static_cast<const FUniqueNetIdRepl&>(PlayerState->UniqueId) : nullptr;
    }

    // Swaps Stub at slot Index for Replacement in the controller's class and every class of its with the stub there.
    void SwapStubInSubclasses(int32_t Index, void* Stub, void* Replacement, const std::wstring& HookName)
    {
        UClass* AthenaClass = AFortPlayerControllerAthena::StaticClass();
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Class = *It;
            UObject* ClassDefault = Class->IsChildOf(AthenaClass) ? Class->GetDefaultObject(false) : nullptr;
            void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
            if (VTable && VTable[Index] == Stub)
            {
                Hooks::SwapVTable(HookName, VTable, Index, Replacement);
            }
        }
    }

    // Swaps the stub at Function's _Implementation slot for Replacement in the controller's class and every class of its
    // with the stub there.
    void RestoreImplementation(UFunction* Function, void* Replacement, const wchar_t* Name)
    {
        UClass* AthenaClass = AFortPlayerControllerAthena::StaticClass();
        const int32_t Index = Function ? Finder::ImplementationIndex(Function) : -1;
        UObject* Default = AthenaClass && Index >= 0 ? AthenaClass->GetDefaultObject(false) : nullptr;
        void** DefaultVTable = Default ? *reinterpret_cast<void***>(Default) : nullptr;
        if (!DefaultVTable)
        {
            Log::Error(L"Finder: AFortPlayerControllerAthena::{}_Implementation slot not found", Name);
            return;
        }
        void* const Stub = DefaultVTable[Index];
        if (!Finder::IsStubFunction(reinterpret_cast<uintptr_t>(Stub)))
        {
            Log::Info(L"Finder: AFortPlayerControllerAthena::{}_Implementation (slot 0x{:X}) is native in this build", Name, Index);
            return;
        }
        SwapStubInSubclasses(Index, Stub, Replacement, std::format(L"AFortPlayerControllerAthena::{}_Implementation", Name));
    }

    // The creative profile's plot item by its id: UMcpProfile::FindItem (7.10 0x961C10: the profile's item map, whose
    // FString keys compare case-insensitively), reached through the items themselves, which the profile makes with
    // itself as their outer (as LoadCreativePlots finds them).
    UFortCreativeRealEstatePlotItem* FindPlotItem(UMcpProfile* Profile, const FString& PlotItemId)
    {
        if (!Profile || !UFortCreativeRealEstatePlotItem::StaticClass())
        {
            return nullptr;
        }
        for (TObjectIterator<UFortCreativeRealEstatePlotItem> It; It; ++It)
        {
            if (It->GetOuter() == Profile && _wcsicmp(*It->GetItemId(), *PlotItemId) == 0)
            {
                return *It;
            }
        }
        return nullptr;
    }

    // The function that contains the code reference to Text (its first), or 0.
    uintptr_t FunctionReferencing(const wchar_t* Text)
    {
        const std::vector<uintptr_t> Refs = Finder::StringRefs(Text);
        return Refs.empty() ? 0 : Finder::FunctionStart(Refs.front());
    }

    // Every function with a code reference to Text.
    std::set<uintptr_t> FunctionsReferencing(const wchar_t* Text)
    {
        std::set<uintptr_t> Functions;
        for (const uintptr_t Ref : Finder::StringRefs(Text))
        {
            if (const uintptr_t Function = Finder::FunctionStart(Ref))
            {
                Functions.insert(Function);
            }
        }
        return Functions;
    }

    bool IsExecutable(uintptr_t Address)
    {
        for (const auto& [Begin, End] : Finder::ExecutableSections())
        {
            if (Address >= Begin && Address < End)
            {
                return true;
            }
        }
        return false;
    }

    // The queue step that calls Function and returns a member of the object it was called on (lea rax, [rbx+disp32]
    // right after the call): UPlaysetLevelStreamComponent's LoadPlaysetAsyncNoCallback and UnloadPlaysetAsyncNoCallback
    // (7.00 0x1325850, 0x1338210; 7.10 0x1364750, 0x13775D0, both returning +0x148 / +0x168).
    uintptr_t StepCalling(uintptr_t Function)
    {
        for (const uintptr_t Site : Function ? Finder::CallsTo(Function, false) : std::vector<uintptr_t>())
        {
            const auto* Next = reinterpret_cast<const uint8_t*>(Site + 5);
            const uintptr_t Start = Finder::FunctionStart(Site);
            if (Start && Site - Start < 0x30 && Finder::IsReadable(Next, 3) && Next[0] == 0x48 && Next[1] == 0x8D && (Next[2] & 0xF8) == 0x80 && (Next[2] & 7) != 4)
            {
                return Start;
            }
        }
        // 7.20: the step tail-calls Function, which returns the member itself (0x144B740: jmp LoadPlayset; 0x145B140:
        // cmp qword ptr [rcx+158h], 0; jz; mov dl, 1; jmp UnloadPlayset). Such a stub has no unwind entry, so its start is
        // the nearest 16-aligned address at or before the jmp, within 0x30, whose address a delegate binding takes
        // (lea r64, [rip+X]: the game's own BeginRestrictedPlotLoad and reset bind it). Functions that end in a jmp to it
        // after work of their own (7.20 0x142D6A0, a BeginPlay) are in vtables, not bound.
        for (const uintptr_t Site : Function ? Finder::CallsTo(Function, true) : std::vector<uintptr_t>())
        {
            if (*reinterpret_cast<const uint8_t*>(Site) != 0xE9)
            {
                continue;
            }
            for (uintptr_t Start = Site & ~static_cast<uintptr_t>(0xF); Site - Start < 0x30; Start -= 0x10)
            {
                if (!Finder::AddressRefs(Start).empty())
                {
                    return Start;
                }
            }
        }
        return 0;
    }

    // From 7.00: what an island's load runs on and with (Natives::AFortVolume_GetTaskQueue and the rest), off the game's
    // own reset of an island (AFortVolumeManager::ResetVolumeAndLevelRecord, 0xDA0E70; 7.10 0xDC4320, the function that
    // logs "... Task Queue was not valid and empty") and the steps' own log lines.
    void ResolveIslandLoad()
    {
        const uintptr_t Reset = FunctionReferencing(L"AFortVolumeManager::ResetVolumeAndLevelRecord Task Queue was not valid and empty");
        const std::vector<std::pair<uintptr_t, uintptr_t>> ResetCalls = Reset ? Finder::DecodedDirectCalls(Reset) : std::vector<std::pair<uintptr_t, uintptr_t>>();
        // AFortVolume::GetTaskQueue: the reset's first call, the getter that reads TaskQueue (made on first use).
        const int32_t TaskQueueOffset = AFortVolume::TaskQueueOffset();
        if (!ResetCalls.empty() && TaskQueueOffset > 0 && Finder::ReadsMemberAt(ResetCalls.front().second, TaskQueueOffset))
        {
            Natives::AFortVolume_GetTaskQueue = reinterpret_cast<decltype(Natives::AFortVolume_GetTaskQueue)>(ResetCalls.front().second);
        }
        // UAsyncTaskQueue::Add: the call the reset makes once per step (7 on 7.00), the one of them that makes an object
        // (the step's UAsyncTask). The reset's other repeated calls are the delegate's allocations and the weak pointers.
        std::map<uintptr_t, int32_t> Counts;
        for (const auto& [Site, Target] : ResetCalls)
        {
            ++Counts[Target];
        }
        const uintptr_t ConstructObject = reinterpret_cast<uintptr_t>(Natives::StaticConstructObject_Internal);
        for (const auto& [Target, Count] : Counts)
        {
            if (Count < 3 || !ConstructObject || Target == reinterpret_cast<uintptr_t>(Natives::AFortVolume_GetTaskQueue))
            {
                continue;
            }
            const std::vector<std::pair<uintptr_t, uintptr_t>> Inner = Finder::DecodedDirectCalls(Target);
            if (std::any_of(Inner.begin(), Inner.end(), [ConstructObject](const auto& Call) { return Call.second == ConstructObject; }))
            {
                Natives::UAsyncTaskQueue_Add = reinterpret_cast<decltype(Natives::UAsyncTaskQueue_Add)>(Target);
                break;
            }
        }
        // The volume's clear: the step the reset binds (lea) that returns OnFinishVolumeClear.
        const int32_t ClearOffset = AFortVolume::OnFinishVolumeClearOffset();
        for (const auto& [Site, Taken] : Reset && ClearOffset > 0 ? Finder::DecodedAddressesTaken(Reset) : std::vector<std::pair<uintptr_t, uintptr_t>>())
        {
            if (IsExecutable(Taken) && Finder::FunctionStart(Taken) == Taken && Finder::ReadsMemberAt(Taken, ClearOffset))
            {
                Natives::AFortVolume_ClearVolumeOnServer = reinterpret_cast<decltype(Natives::AFortVolume_ClearVolumeOnServer)>(Taken);
                break;
            }
        }
        // The queue's start: cmp qword ptr [rcx+CurrentTask], 0; je ExecuteNextTask, where ExecuteNextTask is one of the
        // two functions that bind "&UAsyncTaskQueue::OnAsyncTaskFinished" (the other is OnAsyncTaskFinished itself).
        const std::set<uintptr_t> NextTask = FunctionsReferencing(L"&UAsyncTaskQueue::OnAsyncTaskFinished");
        const int32_t CurrentTaskOffset = UAsyncTaskQueue::CurrentTaskOffset();
        uintptr_t ExecuteNextTask = 0;
        for (const auto& [Begin, End] : !NextTask.empty() && CurrentTaskOffset > 0 && CurrentTaskOffset < 0x80 ? Finder::ExecutableSections() : std::vector<std::pair<uintptr_t, uintptr_t>>())
        {
            const auto* Bytes = reinterpret_cast<const uint8_t*>(Begin);
            for (size_t Index = 0; Index + 11 <= End - Begin && !Natives::UAsyncTaskQueue_Start; ++Index)
            {
                if (Bytes[Index] != 0x48 || Bytes[Index + 1] != 0x83 || Bytes[Index + 2] != 0x79 || Bytes[Index + 3] != CurrentTaskOffset || Bytes[Index + 4] != 0
                    || Bytes[Index + 5] != 0x0F || Bytes[Index + 6] != 0x84)
                {
                    continue;
                }
                const uintptr_t Site = Begin + Index;
                const uintptr_t Target = Finder::RelativeTarget(Site + 5, 2, 6);
                if (NextTask.contains(Target))
                {
                    ExecuteNextTask = Target;
                    Natives::UAsyncTaskQueue_Start = reinterpret_cast<decltype(Natives::UAsyncTaskQueue_Start)>(Site);
                }
            }
        }
        // The steps of the island's own: the plot's playset loaded, and the stream component's level in and out.
        Natives::UFortLevelSaveComponent_LoadPlaysetAsync = reinterpret_cast<decltype(Natives::UFortLevelSaveComponent_LoadPlaysetAsync)>(
            FunctionReferencing(L"Start playset async load for user %s, cloudsave=%s."));
        Natives::UFortLevelSaveComponent_LoadPlaysetAsyncTakesPlayset = Natives::UFortLevelSaveComponent_LoadPlaysetAsync
            && Finder::KeepsSecondArgument(reinterpret_cast<uintptr_t>(Natives::UFortLevelSaveComponent_LoadPlaysetAsync));
        const uintptr_t LoadPlayset = FunctionReferencing(L"UPlaysetLevelStreamComponent::LoadPlayset Error: no owner for %s");
        const uintptr_t UnloadPlayset = FunctionReferencing(L"UPlaysetLevelStreamComponent::UnloadPlayset %s");
        Natives::UPlaysetLevelStreamComponent_LoadPlaysetAsyncNoCallback = reinterpret_cast<decltype(Natives::UPlaysetLevelStreamComponent_LoadPlaysetAsyncNoCallback)>(
            StepCalling(LoadPlayset));
        Natives::UPlaysetLevelStreamComponent_UnloadPlaysetAsyncNoCallback = reinterpret_cast<decltype(Natives::UPlaysetLevelStreamComponent_UnloadPlaysetAsyncNoCallback)>(
            StepCalling(UnloadPlayset));
        // 7.30 (CL-4834550) has no NoCallback steps: the island's own steps (BeginRestrictedPlotLoad 0x106A880, the reset
        // 0x108F660) bind LoadPlayset (lea rax, 0x148A5A0) and UnloadPlayset (lea rax, 0x149A330) themselves, the unload
        // with a true payload (mov byte ptr [rbx+20h], 1 at 0x106ACEC). LoadPlayset is the load step; the unload step is
        // UnloadPlayset(true), which returns null without a playset (+0x158) as 7.20's step did (0x145B140).
        if (!Natives::UPlaysetLevelStreamComponent_LoadPlaysetAsyncNoCallback && LoadPlayset && !Finder::AddressRefs(LoadPlayset).empty())
        {
            Natives::UPlaysetLevelStreamComponent_LoadPlaysetAsyncNoCallback = reinterpret_cast<decltype(Natives::UPlaysetLevelStreamComponent_LoadPlaysetAsyncNoCallback)>(LoadPlayset);
        }
        if (!Natives::UPlaysetLevelStreamComponent_UnloadPlaysetAsyncNoCallback && UnloadPlayset && !Finder::AddressRefs(UnloadPlayset).empty())
        {
            Natives::UPlaysetLevelStreamComponent_UnloadPlayset = reinterpret_cast<decltype(Natives::UPlaysetLevelStreamComponent_UnloadPlayset)>(UnloadPlayset);
        }
        // UFortLevelSaveComponent::ResetLevelRecord: the function that sets the island file's "last-reset" meta, one of
        // the component's virtuals.
        const uintptr_t ResetRecord = FunctionReferencing(L"last-reset");
        UObject* SaveDefault = UFortLevelSaveComponent::StaticClass() ? UFortLevelSaveComponent::StaticClass()->GetDefaultObject(false) : nullptr;
        const int32_t ResetSlot = ResetRecord && SaveDefault ? Finder::VTableIndex(*reinterpret_cast<void***>(SaveDefault), reinterpret_cast<void*>(ResetRecord), 0x200) : -1;
        if (ResetSlot >= 0)
        {
            Natives::UFortLevelSaveComponent_ResetLevelRecord = reinterpret_cast<decltype(Natives::UFortLevelSaveComponent_ResetLevelRecord)>(ResetRecord);
        }
        // ULevelSaveRecord's end of a read: the function that logs "read_failed" and isn't ReadFromDisk (which logs
        // "ULevelSaveRecord::ReadFromDisk Invalid File" and calls ReadFromBuffer itself).
        const uintptr_t ReadFromDisk = FunctionReferencing(L"ULevelSaveRecord::ReadFromDisk Invalid File");
        for (const uintptr_t Function : FunctionsReferencing(L"read_failed"))
        {
            const std::vector<std::pair<uintptr_t, uintptr_t>> Inner = Finder::DecodedDirectCalls(Function);
            const uintptr_t ReadFromBuffer = reinterpret_cast<uintptr_t>(Natives::ULevelSaveRecord_ReadFromBuffer);
            if (Function != ReadFromDisk && std::none_of(Inner.begin(), Inner.end(), [ReadFromBuffer](const auto& Call) { return Call.second == ReadFromBuffer; }))
            {
                Natives::ULevelSaveRecord_NotifyFinishedRead = reinterpret_cast<decltype(Natives::ULevelSaveRecord_NotifyFinishedRead)>(Function);
                break;
            }
        }

        const auto Address = [](const void* Function) { return Function ? reinterpret_cast<uintptr_t>(Function) - Finder::ImageBase() : 0; };
        Log::Info(L"Finder: Creative's island load: AFortVolume::GetTaskQueue 0x{:X}, UAsyncTaskQueue::Add 0x{:X}, its start 0x{:X} (ExecuteNextTask 0x{:X}), the volume's clear 0x{:X}, "
            L"UFortLevelSaveComponent::LoadPlaysetAsync 0x{:X}{}, UPlaysetLevelStreamComponent::LoadPlaysetAsyncNoCallback 0x{:X} and UnloadPlaysetAsyncNoCallback 0x{:X}, "
            L"UFortLevelSaveComponent::ResetLevelRecord 0x{:X} (slot 0x{:X}), ULevelSaveRecord's end of a read 0x{:X}",
            Address(reinterpret_cast<const void*>(Natives::AFortVolume_GetTaskQueue)), Address(reinterpret_cast<const void*>(Natives::UAsyncTaskQueue_Add)),
            Address(reinterpret_cast<const void*>(Natives::UAsyncTaskQueue_Start)), ExecuteNextTask ? ExecuteNextTask - Finder::ImageBase() : 0,
            Address(reinterpret_cast<const void*>(Natives::AFortVolume_ClearVolumeOnServer)), Address(reinterpret_cast<const void*>(Natives::UFortLevelSaveComponent_LoadPlaysetAsync)),
            Natives::UFortLevelSaveComponent_LoadPlaysetAsyncTakesPlayset ? L" (takes the playset)" : L"",
            Address(reinterpret_cast<const void*>(Natives::UPlaysetLevelStreamComponent_LoadPlaysetAsyncNoCallback)),
            Address(Natives::UPlaysetLevelStreamComponent_UnloadPlaysetAsyncNoCallback ? reinterpret_cast<const void*>(Natives::UPlaysetLevelStreamComponent_UnloadPlaysetAsyncNoCallback)
                : reinterpret_cast<const void*>(Natives::UPlaysetLevelStreamComponent_UnloadPlayset)),
            Address(reinterpret_cast<const void*>(Natives::UFortLevelSaveComponent_ResetLevelRecord)), ResetSlot, Address(reinterpret_cast<const void*>(Natives::ULevelSaveRecord_NotifyFinishedRead)));
        if (!Natives::AFortVolume_GetTaskQueue || !Natives::UAsyncTaskQueue_Add || !Natives::UAsyncTaskQueue_Start || !Natives::AFortVolume_ClearVolumeOnServer
            || !Natives::UFortLevelSaveComponent_LoadPlaysetAsync || !Natives::UPlaysetLevelStreamComponent_LoadPlaysetAsyncNoCallback
            || (!Natives::UPlaysetLevelStreamComponent_UnloadPlaysetAsyncNoCallback && !Natives::UPlaysetLevelStreamComponent_UnloadPlayset)
            || !Natives::UFortLevelSaveComponent_ResetLevelRecord || !Natives::ULevelSaveRecord_NotifyFinishedRead)
        {
            Log::Error(L"Finder: Creative's island load is missing a part, so players' islands don't load (see the line above)");
        }
    }
}

bool AFortPlayerControllerAthena::IsCreativeMatch(const UObject* WorldContextObject)
{
    if (!AFortAthenaCreativePortal::StaticClass() || !UFortPlaylistAthena::HasEnableCreativeMode() || !WorldContextObject)
    {
        return false;
    }
    UWorld* World = WorldContextObject->GetWorld();
    AFortGameStateAthena* GameState = World ? Cast<AFortGameStateAthena>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    UFortPlaylistAthena* Playlist = GameState && AFortGameStateAthena::HasCurrentPlaylist() ? GameState->GetCurrentPlaylist() : nullptr;
    return Playlist && Playlist->bEnableCreativeMode;
}

void AFortPlayerControllerAthena::SetOwnedPortal(AFortAthenaCreativePortal* Portal)
{
    if (!HasOwnedPortal())
    {
        return;
    }
    // The player's rift, which replicates to them. Their island link (CreativePlotLinkedVolume) isn't the rift's: it is
    // the island they are on, set as the rift teleports them onto it (#485: with it set here, the rift counted its owner
    // as on the island while they stood in the hub, "1/1", and the HUD's island fallback,
    // GetCurrentVolumeOrCreativePlotLinkedVolume as 21.40 names it, 7.00 0xCBFCD0, showed the island's memory bar in the
    // hub).
    OwnedPortal = Portal;
    ForceNetUpdate();
}

AFortVolume* AFortPlayerControllerAthena::GetOwnedIsland() const
{
    AFortAthenaCreativePortal* Portal = GetOwnedPortal();
    return Portal ? static_cast<AFortVolume*>(Portal->LinkedVolume) : nullptr;
}

void AFortPlayerControllerAthena::SetCreativePlotLinkedVolume(AFortVolume* Volume)
{
#if WITH_SERVER_CODE
    if (!HasOwnedPortal() || Role != ROLE_Authority)
    {
        return;
    }
    AFortVolume* const Previous = CreativePlotLinkedVolume;
    // Only a change of island goes to the setter (#485). The setter binds the controller to the new island's
    // OnVolumeStateChanged and then unbinds it from the old island's, without checking that they differ (7.00 0xC67A90,
    // 7.10 0xC866B0, 7.20 0xD63370). Given the island the player is already linked to, it took back the binding it had
    // just made. A rift's teleport onto the island runs twice, so the player stayed linked to the island with nothing
    // bound: a game's start (AFortMinigame::StartMinigame -> AFortVolumeManager::EnterReadOnlyForVolume) never reached
    // their OnMinigameVolumeStateChanged and OnCreativePermissionsChanged, and Athena_PlayerController's
    // PermissionsChanged -> HandlePermissions never ran RevokeItem(CreativeTool): the phone stayed.
    if (Volume == Previous)
    {
        return;
    }
    if (Natives::AFortPlayerControllerAthena_SetCreativePlotLinkedVolume)
    {
        Natives::AFortPlayerControllerAthena_SetCreativePlotLinkedVolume(this, Volume);
    }
    else
    {
        CreativePlotLinkedVolume = Volume;
    }
    if (Volume)
    {
        RecordCreativeIslandHistory(Volume);
    }
    // The setter refreshes the old island's rifts (its LinkedPortals, 7.20 0xD63370); the new island's count the player
    // once they are refreshed too. A rift's CurrentPopulation is the controllers linked to the rift's LinkedVolume
    // (AFortAthenaCreativePortal's refresh, 7.20 0xCB8CC0). The hub's rift of an island is linked to it from the map
    // (its LinkedVolume), but it isn't always in the island's LinkedPortals (transient, filled at run time), and a rift
    // left out kept its old count: a player on their own island showed 0 on their hub rift (#485, 7.20). So every rift
    // of the portal manager whose island is the old or the new one is refreshed, and replicated with its new count.
    std::vector<AFortAthenaCreativePortal*> Rifts;
    const auto AddRift = [&Rifts](AFortAthenaCreativePortal* Portal)
    {
        if (Portal && std::find(Rifts.begin(), Rifts.end(), Portal) == Rifts.end())
        {
            Rifts.push_back(Portal);
        }
    };
    if (Volume && AFortVolume::HasLinkedPortals())
    {
        for (AFortAthenaCreativePortal* Portal : static_cast<const TArray<AFortAthenaCreativePortal*>&>(Volume->LinkedPortals))
        {
            AddRift(Portal);
        }
    }
    if (AFortCreativePortalManager* Manager = (Volume || Previous) ? AFortCreativePortalManager::Get(this) : nullptr)
    {
        for (AFortAthenaCreativePortal* Portal : static_cast<const TArray<AFortAthenaCreativePortal*>&>(Manager->AllPortals))
        {
            AFortVolume* const Island = Portal ? static_cast<AFortVolume*>(Portal->LinkedVolume) : nullptr;
            if (Island && (Island == Volume || Island == Previous))
            {
                AddRift(Portal);
            }
        }
    }
    for (AFortAthenaCreativePortal* Portal : Rifts)
    {
        Portal->RefreshPortal();
        reinterpret_cast<AActor*>(Portal)->ForceNetUpdate();
    }
    ForceNetUpdate();
    Log::Info(L"Creative: {} is {}", PlayerNameOf(this), Volume ? std::format(L"on the island {}", Volume->GetName().ToStdString()) : std::wstring(L"in the hub"));
#endif
}

void AFortPlayerControllerAthena::LoadPlotForPortal(AFortAthenaCreativePortal* Portal, const FString& PlotItemId)
{
    if (Portal && Natives::AFortPlayerControllerAthena_ServerLoadPlotForPortal)
    {
        Natives::AFortPlayerControllerAthena_ServerLoadPlotForPortal(this, Portal, PlotItemId);
    }
    else if (Portal && bServerLoadPlotForPortalRestored)
    {
        ServerLoadPlotForPortal_Implementation(Portal, PlotItemId);
    }
}

bool AFortPlayerControllerAthena::ServerLoadPlotForPortal_Validate(AFortAthenaCreativePortal* Portal, const FString& PlotItemId)
{
#if WITH_SERVER_CODE
    // 7.10's (0xC84F20): a rift with an island space (its LinkedVolume, 0xBCA0C0), an item id, and a rift that is no
    // player's building or the player's own (ABuildingActor::GetOwningPlayerState, 0xD11970, against their player
    // state). False rejects the RPC (RPC_ValidateFailed), which disconnects the client.
    if (!Portal || !static_cast<AFortVolume*>(Portal->LinkedVolume) || PlotItemId.IsEmpty())
    {
        return false;
    }
    AFortPlayerState* RiftOwner = Portal->GetOwningPlayerState();
    APlayerState* PlayerState = static_cast<APlayerState*>(reinterpret_cast<AController*>(this)->PlayerState);
    return !RiftOwner || reinterpret_cast<UObject*>(RiftOwner) == reinterpret_cast<UObject*>(PlayerState);
#else
    return true;
#endif
}

void AFortPlayerControllerAthena::ServerLoadPlotForPortal_Implementation(AFortAthenaCreativePortal* Portal, const FString& PlotItemId)
{
#if WITH_SERVER_CODE
    // 7.10's (0xC84FC0): the rift's owner checked again, the plot item found in the player's creative profile, its
    // island loaded into the rift for the player, then the plot made their last used one.
    if (!Portal || PlotItemId.IsEmpty())
    {
        return;
    }
    AFortPlayerState* RiftOwner = Portal->GetOwningPlayerState();
    APlayerState* PlayerState = static_cast<APlayerState*>(reinterpret_cast<AController*>(this)->PlayerState);
    if (RiftOwner && reinterpret_cast<UObject*>(RiftOwner) != reinterpret_cast<UObject*>(PlayerState))
    {
        Log::Warning(L"Creative: {} can't load an island into {}: it is another player's", PlayerNameOf(this), Portal->GetName().ToStdString());
        return;
    }
    // The profile's item by that id (UMcpProfile::FindItem, 0x961C10), when it is a plot with a template
    // (UFortAccountItem::GetItemDefinition, slot 0x73).
    UMcpProfile* Profile = GetCreativeProfile();
    UFortCreativeRealEstatePlotItem* PlotItem = FindPlotItem(Profile, PlotItemId);
    if (!PlotItem || !static_cast<UObject*>(PlotItem->ItemDefinition))
    {
        Log::Warning(L"Creative: {} picked the island {}, which {}", PlayerNameOf(this), PlotItemId.ToStdString(),
            !Profile ? L"the server can't look up: it has no creative profile for them" : !PlotItem ? L"isn't a CreativePlot item of their creative profile" : L"has no item definition");
        return;
    }
    // The player's id (0x12C45F0: their player state's UniqueId on a server), which the rift and its island's save keep.
    const FUniqueNetIdRepl* UserId = UniqueIdOf(this);
    if (!UserId)
    {
        return;
    }
    Portal->LoadPlotForUser(*UserId, PlotItem);
    // The plot becomes the player's last used one, here and in their profile's last_used_plot (SetLastUsedCreativePlot,
    // 0x1608550), unless it already is (FString's case-insensitive compare); the plot item's id is its GetInstanceId
    // (slot 0x83).
    const FString ItemId = PlotItem->GetItemId();
    if (Member_LastUsedCreativeIsland.IsAvailable() && _wcsicmp(*static_cast<FString&>(LastUsedCreativeIsland), *ItemId) != 0)
    {
        UFortMcpProfileCreative* CreativeProfile = Cast<UFortMcpProfileCreative>(static_cast<UObject*>(Profile));
        if (!CreativeProfile || !CreativeProfile->SetLastUsedCreativePlot(ItemId))
        {
            Log::Warning(L"Creative: {}'s last used island isn't saved to their profile: UFortMcpProfileCreative::SetLastUsedCreativePlot wasn't found", PlayerNameOf(this));
        }
        LastUsedCreativeIsland = ItemId;
    }
    Log::Info(L"Creative: {} loads their island {} into {}", PlayerNameOf(this), ItemId.ToStdString(), Portal->GetName().ToStdString());
#endif
}

void AFortPlayerControllerAthena::ClaimCreativePortal()
{
#if WITH_SERVER_CODE
    // A player comes into a Creative match: the hub's rift for them, which their last used island loads into (the
    // plot the game keeps as LastUsedCreativeIsland, from the creative profile's last_used_plot; their first island
    // when they have none yet) once LoadCreativePlots has their islands. They can pick another at the rift ("Set Rift
    // Destination", ServerLoadPlotForPortal), from the CreativeIslands it fills.
    if (Role != ROLE_Authority || !IsCreativeMatch(this) || GetOwnedPortal())
    {
        return;
    }
    // 7.10's Publish Island is for the players who may publish (FortPlayerControllerAthenaCreativeIslands.cpp).
    QueryPublishingPermission();
    AFortCreativePortalManager* Manager = AFortCreativePortalManager::Get(this);
    AFortAthenaCreativePortal* Portal = Manager ? Manager->ClaimPortal(this) : nullptr;
    if (!Portal)
    {
        if (!Manager)
        {
            Log::Warning(L"Creative: {} has no rift: the world has no AFortCreativePortalManager", PlayerNameOf(this));
        }
        return;
    }
    UMcpProfile* Profile = GetCreativeProfile();
    if (!Profile)
    {
        UWorld* World = GetWorld();
        UFortGameInstance* GameInstance = World ? Cast<UFortGameInstance>(reinterpret_cast<UObject*>(static_cast<UGameInstance*>(World->OwningGameInstance))) : nullptr;
        Log::Warning(L"Creative: the server has no creative profile for {} (the registration gives one when UFortGameInstance::bCreativeModeProfileEnabled is set: {}), so the "
            L"game can't find their islands' plot items", PlayerNameOf(this),
            GameInstance && UFortGameInstance::HasCreativeModeProfileEnabled() ? (GameInstance->bCreativeModeProfileEnabled ? L"set" : L"not set") : L"unknown");
        return;
    }
    // The profile was queried as the player registered (QueryProfile?profileId=creative); its answer may come before or
    // after this. Its items arriving (and any later change to them) broadcast its ItemsUpdatedDelegate, which loads the
    // islands and the last used one into the rift; an answer that is already in is read now.
    if (UMcpProfile::FOnItemsUpdated* ItemsUpdated = Profile->GetItemsUpdatedDelegate())
    {
        ItemsUpdated->AddUObject(this, &AFortPlayerControllerAthena::OnCreativeProfileItemsUpdated);
    }
    else
    {
        Log::Warning(L"Creative: UMcpProfile's ItemsUpdatedDelegate wasn't found, so {}'s islands are only read now, as they come in", PlayerNameOf(this));
    }
    if (Profile->ProfileRevision < 0)
    {
        Log::Info(L"Creative: {}'s creative profile hasn't come in yet; their islands load into their rift when it does", PlayerNameOf(this));
        return;
    }
    LoadCreativePlots();
    LoadLastUsedPlotIntoPortal();
#endif
}

UMcpProfile* AFortPlayerControllerAthena::GetCreativeProfile() const
{
    UObject* Profile = AFortPlayerController::HasCreativeModeProfile() ? static_cast<UObject*>(CreativeModeProfile) : nullptr;
    return Profile && UMcpProfile::StaticClass() && Profile->IsA(UMcpProfile::StaticClass()) ? static_cast<UMcpProfile*>(Profile) : nullptr;
}

void AFortPlayerControllerAthena::LoadCreativePlots()
{
#if WITH_SERVER_CODE
    // 7.00's client has no code that writes CreativeIslands (only the controller's constructor and destructor touch it)
    // or reads the profile's last_used_plot (its name has no reference): the server's half, which 21.40 still has as
    // AFortPlayerControllerAthena::LoadCreativePlots. The plots are the creative profile's CreativePlot items, which the
    // profile makes with itself as their outer; their order is their number (islandIndex, "Island 1" first).
    UMcpProfile* Profile = GetCreativeProfile();
    if (!Profile || !Member_CreativeIslands.IsAvailable() || !UFortCreativeRealEstatePlotItem::StaticClass())
    {
        return;
    }
    std::vector<UFortCreativeRealEstatePlotItem*> Plots;
    for (TObjectIterator<UFortCreativeRealEstatePlotItem> It; It; ++It)
    {
        if (It->GetOuter() == Profile)
        {
            Plots.push_back(*It);
        }
    }
    std::stable_sort(Plots.begin(), Plots.end(), [](UFortCreativeRealEstatePlotItem* A, UFortCreativeRealEstatePlotItem* B) { return A->IslandIndex < B->IslandIndex; });

    TStructArray<FCreativeIslandData>& Islands = CreativeIslands;
    Islands.Empty();
    std::vector<std::wstring> Ids;
    for (UFortCreativeRealEstatePlotItem* Plot : Plots)
    {
        // An item a later whole-profile answer replaced lingers until the next garbage collection: one island per id.
        const FString Id = Plot->GetItemId();
        if (Id.ToStdString().empty() || std::find(Ids.begin(), Ids.end(), Id.ToStdString()) != Ids.end())
        {
            continue;
        }
        FText Name = Plot->GetIslandDisplayName();
        FCreativeIslandData& Island = Islands.AddDefaulted();
        if (Name.HasTextData())
        {
            Island.IslandName = std::move(Name);
        }
        Island.McpId = Id;
        Ids.push_back(Id.ToStdString());
    }

    // The profile's last_used_plot stat (the server's SetLastUsedCreativePlot sets it as the player picks an island at
    // their rift), when it is one of their islands.
    FString LastUsed;
    const TSharedPtr<FJsonValue, ESPMode::Fast> LastUsedStat = Profile->GetStat(FString(L"last_used_plot"));
    if (LastUsedStat.IsValid())
    {
        LastUsedStat->TryGetString(LastUsed);
    }
    const std::wstring LastUsedId = LastUsed.ToStdString();
    const bool bLastUsedIsIsland = std::find(Ids.begin(), Ids.end(), LastUsedId) != Ids.end();
    if (bLastUsedIsIsland && Member_LastUsedCreativeIsland.IsAvailable())
    {
        LastUsedCreativeIsland = LastUsed;
    }
    ForceNetUpdate();
    Log::Info(L"Creative: {} has {} island(s) in their creative profile (revision {}), last used {}", PlayerNameOf(this), Ids.size(), static_cast<int64_t>(Profile->ProfileRevision),
        bLastUsedIsIsland ? LastUsedId : LastUsedId.empty() ? std::wstring(L"none yet") : std::format(L"{} (not one of them)", LastUsedId));
    if (Ids.empty())
    {
        Log::Warning(L"Creative: {}'s creative profile has no CreativePlot items, so they have no island to load (the backend gives them once the account may play Creative)",
            PlayerNameOf(this));
    }
#endif
}

void AFortPlayerControllerAthena::LoadLastUsedPlotIntoPortal()
{
#if WITH_SERVER_CODE
    AFortAthenaCreativePortal* Portal = GetOwnedPortal();
    AFortVolume* Volume = Portal ? static_cast<AFortVolume*>(Portal->LinkedVolume) : nullptr;
    UFortLevelSaveComponent* SaveComponent = UFortLevelSaveComponent::GetForVolume(reinterpret_cast<AActor*>(Volume));
    if (!Portal || (SaveComponent && static_cast<UFortCreativeRealEstatePlotItem*>(SaveComponent->LoadedPlot)))
    {
        return;
    }
    FString PlotItemId = Member_LastUsedCreativeIsland.IsAvailable() ? static_cast<FString>(LastUsedCreativeIsland) : FString();
    if (PlotItemId.ToStdString().empty() && Member_CreativeIslands.IsAvailable())
    {
        TStructArray<FCreativeIslandData>& Islands = CreativeIslands;
        if (Islands.Num() > 0)
        {
            PlotItemId = Islands[0].McpId;
        }
    }
    if (PlotItemId.ToStdString().empty())
    {
        Log::Info(L"Creative: {}'s rift waits for them to pick an island (they have none)", PlayerNameOf(this));
        return;
    }
    Log::Info(L"Creative: {}'s {} island {} loads into their rift", PlayerNameOf(this),
        Member_LastUsedCreativeIsland.IsAvailable() && !static_cast<FString>(LastUsedCreativeIsland).ToStdString().empty() ? L"last used" : L"first", PlotItemId.ToStdString());
    LoadPlotForPortal(Portal, PlotItemId);
#endif
}

void AFortPlayerControllerAthena::OnCreativeProfileItemsUpdated(int64_t UpdateArgument)
{
#if WITH_SERVER_CODE
    LoadCreativePlots();
    LoadLastUsedPlotIntoPortal();
#endif
}

void AFortPlayerControllerAthena::ReleaseCreativePortal(AFortPlayerState* PlayerState)
{
#if WITH_SERVER_CODE
    AActor* StateActor = reinterpret_cast<AActor*>(PlayerState);
    if (!StateActor || !IsCreativeMatch(StateActor))
    {
        return;
    }
    if (AFortCreativePortalManager* Manager = AFortCreativePortalManager::Get(StateActor))
    {
        Manager->ReleasePlayerPortal(PlayerState);
    }
#endif
}

AFortVolume* AFortPlayerControllerAthena::GetCreativeVolume()
{
    AFortVolume* Own = GetOwnedIsland();
    APawn* Pawn = GetPawn();
    AFortVolumeManager* Manager = Pawn ? AFortVolumeManager::Get(this) : nullptr;
    AFortVolume* Here = Manager ? Manager->GetVolumeForActor(reinterpret_cast<AActor*>(Pawn)) : nullptr;
    return Here ? Here : Own;
}

bool AFortPlayerControllerAthena::TeleportPawnTo(const FVector& Location, const FRotator& Rotation, const wchar_t* Where)
{
    AActor* Pawn = reinterpret_cast<AActor*>(GetPawn());
    const bool bMoved = Pawn && Pawn->TeleportTo(Location, Rotation);
    Log::Info(L"Creative: {} {} {} ({:.0f}, {:.0f}, {:.0f})", PlayerNameOf(this), bMoved ? L"goes to" : L"couldn't go to", Where, Location.X, Location.Y, Location.Z);
    return bMoved;
}

bool AFortPlayerControllerAthena::HasDelayedTeleport()
{
    // 7.00 has all of it but the HUD's text (6.02+ have the respawn preparation; bDelayedTeleporting is Creative's).
    return Member_bDelayedTeleporting.IsAvailable() && AFortPlayerStateAthena::HasRespawnData() && FindStaticFunction_ClientStartRespawnPreparation();
}

void AFortPlayerControllerAthena::ClientStartRespawnPreparation(const FVector& RespawnLoc, const FRotator& RespawnRot, float RespawnCameraDist, FName InRespawnCameraBehavior, const FText* HUDReasonText)
{
    Reflection::FParams Params(FindStaticFunction_ClientStartRespawnPreparation(), true);
    if (!Params.Set(L"RespawnLoc", RespawnLoc) || !Params.Set(L"RespawnRot", RespawnRot) || !Params.Set(L"RespawnCameraDist", RespawnCameraDist)
        || !Params.Set(L"InRespawnCameraBehavior", InRespawnCameraBehavior))
    {
        return;
    }
    if (HUDReasonText && Params.Has(L"HUDReasonText"))
    {
        Params.Set(L"HUDReasonText", *HUDReasonText);
    }
    Params.Call(this);
}

bool AFortPlayerControllerAthena::StartDelayedTeleport(const FVector& Location, const FRotator& Rotation, const wchar_t* CameraBehavior, const FText* HUDReasonText)
{
#if WITH_SERVER_CODE
    AFortPlayerStateAthena* AthenaState = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(reinterpret_cast<AController*>(this)->PlayerState)));
    if (!HasDelayedTeleport() || !AthenaState || !GetWorld())
    {
        return false;
    }
    // TeleportVehicle's order for each of the vehicle's players: ServerSetRespawnData_Implementation (7.20 0xD61FB0, the
    // destination into the player state's RespawnData, camera distance 0), then ClientStartRespawnPreparation. The
    // server RPC runs here, on the server, as the build's own _Implementation; its fields are written directly where it
    // didn't store them.
    Reflection::FParams SetData(FindStaticFunction_ServerSetRespawnData(), true);
    if (SetData.Set(L"RespawnLoc", Location) && SetData.Set(L"RespawnRot", Rotation) && SetData.Set(L"RespawnCameraDist", 0.0f))
    {
        SetData.Call(this);
    }
    FFortRespawnData& RespawnData = AthenaState->RespawnData;
    const FVector Stored = RespawnData.RespawnLocation;
    if (!RespawnData.bRespawnDataAvailable || Stored.X != Location.X || Stored.Y != Location.Y || Stored.Z != Location.Z)
    {
        RespawnData.bClientIsReady = false;
        RespawnData.bServerIsReady = false;
        RespawnData.RespawnLocation = Location;
        RespawnData.RespawnRotation = Rotation;
        RespawnData.bRespawnDataAvailable = true;
    }
    // Without a camera behaviour the caller sends the preparation itself (PreLobbyIslandTeleport).
    if (CameraBehavior)
    {
        ClientStartRespawnPreparation(Location, Rotation, 0.0f, FName(CameraBehavior), HUDReasonText);
    }
    bDelayedTeleporting = true;
    Log::Info(L"Creative: {} sees the travel screen ({}) for ({:.0f}, {:.0f}, {:.0f})", PlayerNameOf(this), CameraBehavior ? CameraBehavior : L"TeleportToLobbyIsland", Location.X, Location.Y,
        Location.Z);
    return true;
#else
    return false;
#endif
}

void AFortPlayerControllerAthena::FinishDelayedTeleportWhenReady(std::function<void(AFortPlayerControllerAthena*, const FVector&, const FRotator&)> OnReady, int32_t Checks)
{
#if WITH_SERVER_CODE
    AFortPlayerStateAthena* AthenaState = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(reinterpret_cast<AController*>(this)->PlayerState)));
    UWorld* World = GetWorld();
    // A respawn after a death meanwhile (RespawnPlayerAfterDeath clears bDelayedTeleporting) has the data now.
    if (!AthenaState || !World || !IsDelayedTeleporting())
    {
        return;
    }
    FFortRespawnData& RespawnData = AthenaState->RespawnData;
    // TeleportVehicle's timer (7.20 0xC9C210): with RespawnData stored, bServerIsReady; until the client is ready too
    // it sets itself again, 0.1 s later. Core's guard, which Epic's loop doesn't have: after 10 s the teleport goes ahead
    // without the client's word (its RespawnCameraData row's SafetyTime, 5 s, turns its camera back on by then), so a
    // client that never answers isn't left behind with bDelayedTeleporting holding every rift off it.
    constexpr float CheckRate = 0.1f;
    constexpr int32_t MaxChecks = 100;
    if (RespawnData.bRespawnDataAvailable)
    {
        RespawnData.bServerIsReady = true;
        if (!(RespawnData.bClientIsReady && RespawnData.bServerIsReady) && Checks < MaxChecks)
        {
            const TWeakObjectPtr<AFortPlayerControllerAthena> WeakThis(this);
            FTimerHandle Handle;
            World->GetTimerManager().SetTimer(Handle, FTimerDelegate::CreateLambda([WeakThis, OnReady, Checks]()
            {
                if (AFortPlayerControllerAthena* Controller = WeakThis.Get())
                {
                    Controller->FinishDelayedTeleportWhenReady(OnReady, Checks + 1);
                }
            }), CheckRate, false);
            return;
        }
        if (Checks >= MaxChecks)
        {
            Log::Warning(L"Creative: {}'s client didn't say it was ready to travel; teleporting anyway", PlayerNameOf(this));
        }
    }
    // The move, with RespawnData still stored (PostLobbyIslandTeleport reads it), then the data cleared as
    // TeleportVehicle's timer does after its move (0xD7C2F0: bRespawnDataAvailable, bClientIsReady, bServerIsReady),
    // and bDelayedTeleporting as RespawnPlayerAfterDeath does.
    if (RespawnData.bRespawnDataAvailable)
    {
        const FVector Location = RespawnData.RespawnLocation;
        const FRotator Rotation = RespawnData.RespawnRotation;
        OnReady(this, Location, Rotation);
    }
    RespawnData.bRespawnDataAvailable = false;
    RespawnData.bClientIsReady = false;
    RespawnData.bServerIsReady = false;
    bDelayedTeleporting = false;
    // The client's camera back on the pawn (#497). Its preparation (7.20 0xD6A1E0, on the RespawnCameraData row's
    // timer) spawns its own ACameraActor at RespawnData's place and views it (SetViewTargetWithBlend); the camera actor
    // is 16:9-constrained, hence the bars on a 4:3 screen. Once ready (0xD38D80: ServerClientIsReadyToRespawn) it only
    // fades back in when it has a pawn (0xD39210 -> 0xD60320): nothing on the client views the pawn again. A respawn
    // after death gets that from the new pawn's possession (RespawnPlayerAfterDeath 0xD60080 -> ServerRestartPlayer
    // -> RestartPlayer -> Possess -> ClientRestart -> SetViewTarget(Pawn)); a teleported pawn is the one the client
    // already has, so the server sends the engine's ClientSetViewTarget, as its camera manager would for a view
    // target change (the server's view target was the pawn all along, so it sends nothing by itself).
    if (APawn* Pawn = GetPawn())
    {
        ClientSetViewTarget(reinterpret_cast<AActor*>(Pawn));
    }
#endif
}

void AFortPlayerControllerAthena::PreLobbyIslandTeleport()
{
#if WITH_SERVER_CODE
    // The hub start ServerTeleportToPlaygroundLobbyIsland stored: the travel screen with the hub's text, the
    // TeleportToLobbyIsland row of RespawnCameraData, and no rift on arrival.
    AFortPlayerStateAthena* AthenaState = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(reinterpret_cast<AController*>(this)->PlayerState)));
    if (!AthenaState || !AFortPlayerStateAthena::HasRespawnData() || !FindStaticFunction_ClientStartRespawnPreparation())
    {
        return;
    }
    const FFortRespawnData& RespawnData = AthenaState->RespawnData;
    ClientStartRespawnPreparation(RespawnData.RespawnLocation, RespawnData.RespawnRotation, 0.0f, FName(L"TeleportToLobbyIsland"), GetClientTravelToCreativeHubText());
    bDelayedTeleporting = true;
#endif
}

void AFortPlayerControllerAthena::PostLobbyIslandTeleport(AFortPlayerPawn* PlayerPawn)
{
#if WITH_SERVER_CODE
    AFortPlayerStateAthena* AthenaState = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(reinterpret_cast<AController*>(this)->PlayerState)));
    if (!PlayerPawn || !AthenaState || reinterpret_cast<APawn*>(PlayerPawn) != GetPawn())
    {
        return;
    }
    const FFortRespawnData& RespawnData = AthenaState->RespawnData;
    TeleportPawnTo(RespawnData.RespawnLocation, RespawnData.RespawnRotation, L"the Creative hub");
    // The player has left their island: the link goes (CreativePlotLinkedVolume), and the setter recounts the island's
    // rifts. After the move (#569): 8.20's count (AFortAthenaCreativePortal::UpdateStatus, 0xE2F970) takes the
    // controller's GetCurrentVolume first (the volume manager's VolumeActivePlayers, which loses the player when the
    // pawn's overlap with the island ends) and the link only without one, so a recount before the move still counted
    // the player and the island's rift kept "1/x" with nobody on it. 7.x count the link alone.
    SetCreativePlotLinkedVolume(nullptr);
    // The same pawn arrives, where Epic's stripped trip would have given a new one: no island speed or flight in the hub.
    ResetCreativeMovementState();
    // What they picked up or took from the Creative inventory stays on the island (#499): a game's end resets its
    // players (AFortMinigame::ClearPlayerInventory), but a player who leaves for the hub, during a game or outside one,
    // isn't one of its players any more. 7.20 strips both this function and ClearDroppableItems, and nothing in the image
    // calls ClearDroppableItems, so it is called here, where the player arrives in the hub.
    if (FindStaticFunction_ClearDroppableItems())
    {
        if (const int32_t Removed = ClearDroppableItems(); Removed > 0)
        {
            Log::Info(L"Creative: {} leaves {} items behind for the hub", PlayerNameOf(this), Removed);
        }
    }
#endif
}

int32_t AFortPlayerControllerAthena::ClearDroppableItems()
{
#if WITH_SERVER_CODE
    // Core's reconstruction: 7.10 and 7.20 strip the body (the exec is the shared empty one, 7.20 0x83BA30).
    AFortInventory* Inventory = Role == ROLE_Authority ? static_cast<AFortInventory*>(WorldInventory) : nullptr;
    if (!Inventory || !AFortPlayerController::HasInventoryRemovalHook())
    {
        return 0;
    }
    // The game mode's StartingItems stay (on Creative's: the building pieces, the edit tool and wood).
    AFortGameModeAthena* GameMode = AFortGameModeAthena::GetCurrent(this);
    const TStructArray<FItemAndCount>* StartingItems = GameMode ? &static_cast<const TStructArray<FItemAndCount>&>(GameMode->StartingItems) : nullptr;
    const auto IsStartingItem = [StartingItems](const UFortItemDefinition* ItemDefinition)
    {
        for (int32_t Index = 0; StartingItems && Index < StartingItems->Num(); ++Index)
        {
            if (static_cast<UFortItemDefinition*>((*StartingItems)[Index].Item) == ItemDefinition)
            {
                return true;
            }
        }
        return false;
    };

    // The whole stack of each, collected first: a removal changes the inventory's item list.
    std::vector<std::pair<FGuid, int32_t>> Removals;
    FFortItemList& ItemList = Inventory->Inventory;
    TArray<UFortWorldItem*>& Instances = ItemList.ItemInstances;
    for (int32_t Index = 0; Index < Instances.Num(); ++Index)
    {
        FFortItemEntry* Entry = Instances[Index] ? &static_cast<FFortItemEntry&>(Instances[Index]->ItemEntry) : nullptr;
        UFortWorldItemDefinition* ItemDefinition = Entry ? Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(Entry->ItemDefinition)) : nullptr;
        if (ItemDefinition && ItemDefinition->bCanBeDropped && !IsStartingItem(ItemDefinition))
        {
            Removals.emplace_back(Entry->ItemGuid, static_cast<int32_t>(Entry->Count));
        }
    }
    for (const auto& [ItemGuid, Count] : Removals)
    {
        RemoveInventoryItem(ItemGuid, Count, true, true);
    }
    return static_cast<int32_t>(Removals.size());
#else
    return 0;
#endif
}

void AFortPlayerControllerAthena::ResetCreativeMovementState()
{
#if WITH_SERVER_CODE
    // Epic's 7.10-7.30 respawn a game's players for its warmup (AFortMinigame::RespawnMinigamePlayers) and take a player
    // back to the hub (ServerTeleportToPlaygroundLobbyIsland) in server code the builds strip (7.30: exec 0x84BD00, slot
    // 0x442); Core moves the same pawn there instead, so what the old pawn was doing goes with it.
    if (Role != ROLE_Authority)
    {
        return;
    }
    // The Movement Modulator (Toy_Floor_SpeedIncrease): GA_Toy_SpeedAdjust applies GE_Toy_SpeedAdjust (HasDuration, the
    // device's DurationValue, 3 to 120 s; Run/Sprint/Crouched speeds and TopSpeedScale) to the pawn's ability system,
    // which for a player is the player state's (7.30 0x13B8C40 sets the pawn's from it), so the effect outlives a pawn.
    // Loaded only once a device's ability set loaded it: nobody can have it otherwise, and older builds don't have it.
    UClass* const Effect = FindObject<UClass>(nullptr, L"/Game/Abilities/Toys/SpeedBoost/GE_Toy_SpeedAdjust.GE_Toy_SpeedAdjust_C");
    AFortPlayerState* FortPlayerState = Cast<AFortPlayerState>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(reinterpret_cast<AController*>(this)->PlayerState)));
    UAbilitySystemComponent* AbilitySystem = Effect && FortPlayerState
        ? static_cast<UAbilitySystemComponent*>(FortPlayerState->AbilitySystemComponent) : nullptr;
    if (Effect && AbilitySystem && AbilitySystem->GetGameplayEffectCount(Effect, nullptr, false) > 0
        && AbilitySystem->RemoveActiveGameplayEffectBySourceEffect(Effect, nullptr, -1))
    {
        Log::Info(L"Creative: {} loses the Movement Modulator's speed (GE_Toy_SpeedAdjust)", PlayerNameOf(this));
    }

    // Creative flight (#512): AFortPlayerControllerGameplay's StartFly (7.30 0x14384F0) puts the pawn in MOVE_Flying. Its
    // check while flying, OnVerifyAllowedToFly on VerifyAllowedToFlyTimerHandle (every UFortGlobals::
    // FlyVerificationInterval), is stripped (7.30 and 7.40 bind an empty function), so nothing stops it. The game's own
    // ServerCreativeStopFly_Implementation (7.30 0x1436500: a Creative game's StopFly 0x14389C0, MOVE_Falling, the fly
    // tags off, OnCreativeFlyChangedDelegate, the timer cleared) runs here, and ClientCreativeStopFly (the same body) on
    // the player's client, whose movement leads its pawn's.
    UObject* const Self = reinterpret_cast<UObject*>(this);
    UFunction* const ServerStopFly = Self->FindFunction(FName(L"ServerCreativeStopFly", FNAME_Find));
    UFunction* const ClientStopFly = ServerStopFly ? Self->FindFunction(FName(L"ClientCreativeStopFly", FNAME_Find)) : nullptr;
    APawn* const Pawn = ClientStopFly ? GetPawn() : nullptr;
    ACharacter* const Character = Pawn ? Cast<ACharacter>(reinterpret_cast<UObject*>(Pawn)) : nullptr;
    UCharacterMovementComponent* const Movement = Character ? static_cast<UCharacterMovementComponent*>(Character->CharacterMovement) : nullptr;
    constexpr uint8_t MOVE_Flying = 5;
    if (!Movement || Movement->MovementMode != MOVE_Flying)
    {
        return;
    }
    Reflection::FParams ServerParams(ServerStopFly, true);
    ServerParams.Call(this);
    Reflection::FParams ClientParams(ClientStopFly, true);
    ClientParams.Call(this);
    Log::Info(L"Creative: {} stops flying (ServerCreativeStopFly, ClientCreativeStopFly)", PlayerNameOf(this));
#endif
}

void AFortPlayerControllerAthena::MoveToolRemoved()
{
#if WITH_SERVER_CODE
    // 7.40 (CL-5046157) keeps the quickbars on the client, which moves off a held item only when the server queues it a
    // quickbar change (DelayedQuickBarActions). A weapon whose item leaves the inventory is marked pending destroy (on
    // authority, AFortWeapon's inventory handler 0x15BB1F0 sets the flag at 0x15BB2B7 once its item guid is gone), but
    // the queue and the destroy wait for an ability to end on the pawn (AFortPawn's handler 0x13D35F0 -> 0x15A3FF0).
    // Holding the phone runs no ability, so the revoked phone stayed in the player's hands in the hub (#554). The same
    // handler runs here, as the tool is taken; it sends the client the Replace that moves its quickbar off the phone.
    AFortPawn* Pawn = Role == ROLE_Authority ? static_cast<AFortPawn*>(MyFortPawn) : nullptr;
    AFortWeapon* Weapon = Pawn ? static_cast<AFortWeapon*>(Pawn->CurrentWeapon) : nullptr;
    AFortInventory* Inventory = WorldInventory;
    if (!Weapon || !Inventory || Inventory->FindWorldItem(static_cast<FGuid&>(Weapon->ItemEntryGuid)))
    {
        return;
    }
    Weapon->SetPendingDestroyDueToDurability();
    if (Weapon->HandlePendingDestroy())
    {
        Log::Info(L"Creative: {} loses the Creative phone from their hands ({}: its quickbar slot replaced, the weapon destroyed)", PlayerNameOf(this),
            Weapon->GetName().ToStdString());
    }
#endif
}

DEFINE_FUNCTION(AFortPlayerControllerAthena::execMoveToolRemoved)
{
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortPlayerControllerAthena*>(Context)->MoveToolRemoved();
    P_NATIVE_END;
}

namespace
{
    UFunction* CreativeModeEnabledChangedHandler = nullptr;

    // TSoftObjectPtr::Get as Epic's: the weak pointer, else the object the path names if it is in memory. Core's
    // TAssetPtr::Get reads only the weak pointer, which nothing in 9.00 fills for CreativeMoveToolItemDefinition (#683:
    // the grant and the removal found no phone and did nothing).
    UFortWorldItemDefinition* GetCreativeMoveToolItemDefinition(const TAssetPtr<UFortWorldItemDefinition>& Reference)
    {
        if (UFortWorldItemDefinition* Loaded = Reference.Get())
        {
            return Loaded;
        }
        const FStringAssetReference& Path = Reference.ToStringReference();
        return Path.IsValid() ? FindObject<UFortWorldItemDefinition>(nullptr, *Path.ToString()) : nullptr;
    }
}

void AFortPlayerControllerAthena::GrantCreativePhoneItemDefLoadedCallback()
{
#if WITH_SERVER_CODE
    // Core's (no build has the body): the phone once its definition is in, given as 8.51's Athena_PlayerController's
    // GrantItem gives it (GiveItemToInventoryOwner, one, notified, level -1), unless the player has it already, then put
    // in its slot by the owning client.
    UFortWorldItemDefinition* Phone = GetCreativeMoveToolItemDefinition(CreativeMoveToolItemDefinition);
    Reflection::FParams Enabled(FindStaticFunction_IsCreativeModeEnabled(), true);
    if (Role != ROLE_Authority || !Phone || !Enabled.Call(this) || !Enabled.Take<bool>(L"ReturnValue"))
    {
        return;
    }
    AFortInventory* Inventory = WorldInventory;
    bool bHasPhone = false;
    if (Inventory)
    {
        TArray<UFortWorldItem*>& Instances = Inventory->Inventory.ItemInstances;
        for (int32_t Index = 0; Index < Instances.Num() && !bHasPhone; ++Index)
        {
            bHasPhone = Instances[Index] && static_cast<UFortItemDefinition*>(Instances[Index]->ItemEntry.ItemDefinition) == static_cast<UFortItemDefinition*>(Phone);
        }
    }
    if (!bHasPhone)
    {
        UFortKismetLibrary::K2_GiveItemToPlayer(this, Phone, 1, true, -1);
    }
    Log::Info(L"Creative: {} may edit their island: {} the Creative phone ({})", PlayerNameOf(this), bHasPhone ? L"keeps" : L"gets", Phone->GetName().ToStdString());
    Reflection::FParams Slot(FindStaticFunction_ClientEnsureCreativePhoneIsInProperSlot(), true);
    Slot.Call(this);
#endif
}

DEFINE_FUNCTION(AFortPlayerControllerAthena::execGrantCreativePhoneItemDefLoadedCallback)
{
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortPlayerControllerAthena*>(Context)->GrantCreativePhoneItemDefLoadedCallback();
    P_NATIVE_END;
}

void AFortPlayerControllerAthena::HandleCreativeModeEnabledChanged(bool bIsEnabled)
{
#if WITH_SERVER_CODE
    if (Role != ROLE_Authority)
    {
        return;
    }
    const TAssetPtr<UFortWorldItemDefinition>& PhoneReference = CreativeMoveToolItemDefinition;
    if (!bIsEnabled)
    {
        // As 8.51's RevokeItem (K2_RemoveItemFromPlayer, all of them, forced).
        if (UFortWorldItemDefinition* Phone = GetCreativeMoveToolItemDefinition(PhoneReference))
        {
            UFortKismetLibrary::K2_RemoveItemFromPlayer(this, Phone, -1, true);
            Log::Info(L"Creative: {} may no longer edit here: the Creative phone ({}) is taken if they have it", PlayerNameOf(this), Phone->GetName().ToStdString());
        }
        return;
    }
    // Core's: the definition loads at once on the game thread where Epic's waits for the streamable manager; the grant is
    // the game's own callback either way.
    if (PhoneReference.LoadSynchronous())
    {
        Reflection::FParams Grant(FindStaticFunction_GrantCreativePhoneItemDefLoadedCallback(), true);
        Grant.Call(this);
    }
#endif
}

DEFINE_FUNCTION(AFortPlayerControllerAthena::execHandleCreativeModeEnabledChanged)
{
    P_GET_UBOOL(bIsEnabled);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortPlayerControllerAthena*>(Context)->HandleCreativeModeEnabledChanged(bIsEnabled);
    P_NATIVE_END;
}

void AFortPlayerControllerAthena::BindCreativeModeEnabledChanged(AFortPlayerControllerAthena* Controller)
{
#if WITH_SERVER_CODE
    if (Controller && CreativeModeEnabledChangedHandler)
    {
        FMulticastScriptDelegate& Changed = Controller->OnCreativeModeEnabledChangedDelegate;
        Changed.AddUniqueDynamic(Controller, CreativeModeEnabledChangedHandler->GetFName());
    }
#endif
}

void AFortPlayerControllerAthena::ClientCycleQuickBarToCreativeItem(const FGuid& CreativeItemGuid, UFortWorldItemDefinition* CreativeItemDef)
{
    Reflection::FParams Params(FindStaticFunction_ClientCycleQuickBarToCreativeItem(), true);
    if (Params.Set(L"CreativeItemGuid", CreativeItemGuid) && Params.Set(L"CreativeItemDef", CreativeItemDef))
    {
        Params.Call(this);
    }
}

void AFortPlayerControllerAthena::ServerGiveCreativeItem_Implementation(const FFortItemEntry& CreativeItem)
{
#if WITH_SERVER_CODE
    // The Creative inventory's pick (UFortCreativeItemListMenu): the item goes into the player's inventory and their
    // quick bar moves to it (ClientCycleQuickBarToCreativeItem). Only in a Creative match.
    UFortWorldItemDefinition* Definition = Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(CreativeItem.ItemDefinition));
    AFortInventory* Inventory = WorldInventory;
    if (!Definition || !Inventory || !IsCreativeMatch(this))
    {
        Log::Warning(L"Creative: {}'s ServerGiveCreativeItem refused ({})", PlayerNameOf(this), !Definition ? L"no world item definition" : !Inventory ? L"no inventory" : L"not a Creative match");
        return;
    }
    UScriptStruct* EntryStruct = FFortItemEntry::StaticStruct();
    std::vector<uint8_t> Storage(EntryStruct->GetStructureSize(), 0);
    FFortItemEntry* Entry = reinterpret_cast<FFortItemEntry*>(Storage.data());
    EntryStruct->InitializeStruct(Entry);
    EntryStruct->CopyScriptStruct(Entry, &CreativeItem);
    const int32_t Count = std::max<int32_t>(1, Entry->Count);
    // A weapon comes loaded, as Battle Royale's loot does.
    if (Entry->LoadedAmmo <= 0)
    {
        Entry->LoadFullClip();
    }
    std::vector<UFortWorldItem*> NewItems;
    const int32_t Added = Inventory->AddWorldItemCopy(this, *Entry, Count, Count, &NewItems);
    EntryStruct->DestroyStruct(Entry);
    if (Added > 0 && !NewItems.empty() && NewItems.front())
    {
        const FFortItemEntry& NewEntry = NewItems.front()->ItemEntry;
        ClientCycleQuickBarToCreativeItem(NewEntry.ItemGuid, Definition);
    }
    Log::Info(L"Creative: {} takes {} x{} from the Creative inventory ({} added)", PlayerNameOf(this), Definition->GetName().ToStdString(), Count, Added);
#endif
}

void AFortPlayerControllerAthena::ServerReloadCreativePlot_Implementation()
{
#if WITH_SERVER_CODE
    // The island menu's reload: the player's island as it was last saved.
    AFortVolume* Volume = GetOwnedIsland();
    UFortLevelSaveComponent* SaveComponent = UFortLevelSaveComponent::GetForVolume(reinterpret_cast<AActor*>(Volume));
    const FUniqueNetIdRepl* UserId = UniqueIdOf(this);
    if (!SaveComponent || !UserId || !SaveComponent->IsOwnedBy(*UserId))
    {
        Log::Warning(L"Creative: {}'s ServerReloadCreativePlot: they have no island loaded", PlayerNameOf(this));
        return;
    }
    Log::Info(L"Creative: {} reloads their island", PlayerNameOf(this));
    SaveComponent->ReloadPlot();
#endif
}

void AFortPlayerControllerAthena::ServerStopSavingCreativePlot_Implementation()
{
#if WITH_SERVER_CODE
    // The island stops saving on its timer (ULevelSaveBaseComponent::StopPeriodicSaveTimer).
    AFortVolume* Volume = GetOwnedIsland();
    UFortLevelSaveComponent* SaveComponent = UFortLevelSaveComponent::GetForVolume(reinterpret_cast<AActor*>(Volume));
    const FUniqueNetIdRepl* UserId = UniqueIdOf(this);
    if (SaveComponent && UserId && SaveComponent->IsOwnedBy(*UserId))
    {
        SaveComponent->StopPeriodicSaveTimer();
        Log::Info(L"Creative: {}'s island stops saving", PlayerNameOf(this));
    }
#endif
}

void AFortPlayerControllerAthena::ResetMyCurrentCreativePlot_Implementation()
{
#if WITH_SERVER_CODE
    // The island menu's "Island Tools" reset: the island the player is on, when it is theirs, back to its template
    // (AFortVolumeManager::ResetVolumeAndLevelRecord, the game's own). Its next save uploads the empty record.
    AFortVolume* Volume = GetCreativeVolume();
    UFortLevelSaveComponent* SaveComponent = UFortLevelSaveComponent::GetForVolume(reinterpret_cast<AActor*>(Volume));
    const FUniqueNetIdRepl* UserId = UniqueIdOf(this);
    AFortVolumeManager* Manager = Volume ? AFortVolumeManager::Get(this) : nullptr;
    if (!Manager || !SaveComponent || !UserId || !SaveComponent->IsOwnedBy(*UserId))
    {
        Log::Warning(L"Creative: {}'s ResetMyCurrentCreativePlot refused: {}", PlayerNameOf(this), !Volume ? L"not on an island" : L"the island isn't theirs");
        return;
    }
    Manager->ResetVolumeAndLevelRecord(Volume, reinterpret_cast<AFortPlayerController*>(this));
    Log::Info(L"Creative: {} resets their island {}", PlayerNameOf(this), Volume->GetName().ToStdString());
#endif
}

void AFortPlayerControllerAthena::ServerTeleportToPlaygroundIslandDock_Implementation()
{
#if WITH_SERVER_CODE
    // To the dock of the island the player is on (its Playground.IslandDock.Spawn start), else to its own spawn.
    AFortVolume* Volume = GetCreativeVolume();
    APawn* Pawn = GetPawn();
    if (!Volume || !Pawn)
    {
        return;
    }
    const FVector From = reinterpret_cast<AActor*>(Pawn)->GetTransform().GetLocation();
    AFortPlayerStartCreative* Start = Volume->FindPlayerStart(L"Playground.IslandDock.Spawn", From);
    if (!Start)
    {
        Start = Volume->FindPlayerStart(L"Creative.Island.Spawn", From);
    }
    if (!Start)
    {
        Log::Warning(L"Creative: the island {} has no dock for {}", Volume->GetName().ToStdString(), PlayerNameOf(this));
        return;
    }
    const FTransform Transform = Start->GetTransform();
    TeleportPawnTo(Transform.GetLocation(), FRotator(0.0f, Transform.Rotation.Rotator().Yaw, 0.0f), L"the island's dock");
#endif
}

void AFortPlayerControllerAthena::ServerTeleportToPlaygroundLobbyIsland_Implementation()
{
#if WITH_SERVER_CODE
    // Back to the hub (an island's own rift, bReturnToCreativeHub, sends its pawns here): the hub's
    // Playground.LobbyIsland.Spawn start by the player's rift (its PortalIndex), else the nearest to it.
    AFortAthenaCreativePortal* Portal = GetOwnedPortal();
    APawn* Pawn = GetPawn();
    if (!Pawn)
    {
        return;
    }
    const FVector Near = Portal ? Portal->GetTransform().GetLocation() : reinterpret_cast<AActor*>(Pawn)->GetTransform().GetLocation();
    const int32_t PortalIndex = Portal ? static_cast<int32_t>(Portal->PortalIndex) : -1;
    AFortPlayerStartCreative* Best = nullptr;
    float BestDistance = std::numeric_limits<float>::max();
    UClass* StartClass = AFortPlayerStartCreative::StaticClass();
    for (AActor* Actor : StartClass ? UGameplayStatics::GetAllActorsOfClass(this, StartClass) : TArray<AActor*>())
    {
        AFortPlayerStartCreative* Start = static_cast<AFortPlayerStartCreative*>(Actor);
        if (!Start || !Start->HasTag(L"Playground.LobbyIsland.Spawn"))
        {
            continue;
        }
        const float Distance = Start->PortalIndex == PortalIndex && PortalIndex >= 0 ? -1.0f : (Start->GetTransform().GetLocation() - Near).SizeSquared();
        if (Distance < BestDistance)
        {
            BestDistance = Distance;
            Best = Start;
        }
    }
    if (Best)
    {
        const FTransform Transform = Best->GetTransform();
        const FVector Location = Transform.GetLocation();
        const FRotator Rotation(0.0f, Transform.Rotation.Rotator().Yaw, 0.0f);
        // The travel screen first (#497): the hub start into RespawnData, PreLobbyIslandTeleport, and the pawn moves in
        // PostLobbyIslandTeleport once the client is ready. A trip already under way is left to finish.
        if (IsDelayedTeleporting())
        {
            return;
        }
        if (StartDelayedTeleport(Location, Rotation, nullptr, nullptr))
        {
            PreLobbyIslandTeleport();
            const TWeakObjectPtr<AFortPlayerPawn> WeakPawn(Cast<AFortPlayerPawn>(reinterpret_cast<UObject*>(Pawn)));
            FinishDelayedTeleportWhenReady([WeakPawn](AFortPlayerControllerAthena* Controller, const FVector&, const FRotator&)
            {
                Controller->PostLobbyIslandTeleport(WeakPawn.Get());
            });
            return;
        }
        // A build without the respawn preparation: at once (the link after the move, as in PostLobbyIslandTeleport).
        TeleportPawnTo(Location, Rotation, L"the Creative hub");
        SetCreativePlotLinkedVolume(nullptr);
        return;
    }
    // A hub without the tagged starts: the rift's own way to the hub (BGA_IslandPortal's TeleportToLobbyIsland: over the
    // rift, at the hub's StarterIslandTeleportHeight). The link goes after the move, as in PostLobbyIslandTeleport.
    UFunction* TeleportToLobby = Portal ? reinterpret_cast<UObject*>(Portal)->FindFunction(FName(L"TeleportToLobbyIsland", FNAME_Find)) : nullptr;
    Reflection::FParams Params(TeleportToLobby, true);
    const bool bTeleportedToLobby = TeleportToLobby && Params.Set(L"RequestingController", static_cast<AFortPlayerController*>(this)) && Params.Call(Portal);
    SetCreativePlotLinkedVolume(nullptr);
    if (bTeleportedToLobby)
    {
        Log::Info(L"Creative: {} goes to the Creative hub through their rift's TeleportToLobbyIsland", PlayerNameOf(this));
        return;
    }
    Log::Warning(L"Creative: {} can't go back to the hub: no Playground.LobbyIsland.Spawn start and no rift", PlayerNameOf(this));
#endif
}

void AFortPlayerControllerAthena::ServerClaimPortal_Implementation()
{
#if WITH_SERVER_CODE
    // Core's (no build has the body): the hub's portal manager claims a rift for the player, the one they get coming into
    // the match, and their last used island loads into it. Nothing for a player who has one.
    if (GetOwnedPortal())
    {
        Log::Info(L"Creative: {} asks for a rift and has one", PlayerNameOf(this));
        return;
    }
    ClaimCreativePortal();
    Log::Info(L"Creative: {} asks for a rift: {}", PlayerNameOf(this), GetOwnedPortal() ? GetOwnedPortal()->GetName().ToStdString() : std::wstring(L"none free"));
#endif
}

void AFortPlayerControllerAthena::ServerReleasePortal_Implementation()
{
#if WITH_SERVER_CODE
    // Core's (no build has the body): the player's rift goes back to the hub's portal manager, as when they leave.
    AFortAthenaCreativePortal* Portal = GetOwnedPortal();
    ReleaseCreativePortal(reinterpret_cast<AFortPlayerState*>(static_cast<APlayerState*>(reinterpret_cast<AController*>(this)->PlayerState)));
    Log::Info(L"Creative: {} gives back their rift {}", PlayerNameOf(this), Portal ? Portal->GetName().ToStdString() : std::wstring(L"(they had none)"));
#endif
}

AFortMinigame* AFortPlayerControllerAthena::GetMinigame()
{
    Reflection::FParams Params(FindStaticFunction_GetMinigame(), true);
    if (!Params.Call(this))
    {
        return nullptr;
    }
    UObject** Minigame = Params.Get<UObject*>(L"ReturnValue");
    return Minigame && *Minigame ? Cast<AFortMinigame>(*Minigame) : nullptr;
}

void AFortPlayerControllerAthena::ServerToggleAutoRestartMinigame_Implementation()
{
#if WITH_SERVER_CODE
    // Core's reconstruction (no build has the body), in the shape of 7.40's ServerRestartMinigame_Implementation (0xDA7860)
    // and ServerStartMinigame_Implementation (0xDA8720): the island the player is in, when they may run its game, then
    // its minigame's own call. Their check (0xD7E8C0) is the island's edit permission, the component's own
    // DoesPlayerHavePermissionToEdit (0x10A1D80: its owner, anyone on an island without an owner, anyone while its
    // PlotPermissions are Public, else the account ids in its whitelist), with no island or no component allowed, or a
    // ReadOnly island (a published one, VolumeState 1).
    AFortVolume* Volume = GetCreativeVolume();
    UFortLevelSaveComponent* Component = UFortLevelSaveComponent::GetForVolume(reinterpret_cast<AActor*>(Volume));
    const FUniqueNetIdRepl* UserId = UniqueIdOf(this);
    const bool bReadOnly = Volume && static_cast<uint8_t>(Volume->VolumeState) == 1;
    const bool bMayRun = !Volume || bReadOnly || !Component || (UserId && Component->DoesPlayerHavePermissionToEdit(*UserId));
    AFortMinigame* Minigame = bMayRun ? GetMinigame() : nullptr;
    if (!Minigame)
    {
        Log::Info(L"Creative: {}'s auto restart toggle refused: {}", PlayerNameOf(this), bMayRun ? L"no game on their island" : L"the island isn't theirs to run");
        return;
    }
    Minigame->ToggleAutoRestartMinigame();
    Log::Info(L"Creative: {} toggles {}'s auto restart", PlayerNameOf(this), Minigame->GetName().ToStdString());
#endif
}

void AFortPlayerControllerAthena::ServerCreativeSetFlightSpeedIndex_Implementation(int32_t Index)
{
#if WITH_SERVER_CODE
    // Core's (no build has the body): the replicated index the client asked for, applied on the server's pawn by the
    // game's own OnRep (its clamp into FlyingModifiers, the movement component's speed), as the owning client's OnRep
    // applies it there. FlightSpeedWhenEnteredSprint is left as the constructor sets it: nothing in 9.00 reads it.
    FlyingModifierIndex = Index;
    Reflection::FParams Params(FindStaticFunction_OnRep_FlyingModifierIndex(), true);
    Params.Call(this);
#endif
}

void AFortPlayerControllerAthena::ServerCreativeSetFlightSprint_Implementation(bool bSprint)
{
#if WITH_SERVER_CODE
    // Core's (no build has the body): as ServerCreativeSetFlightSpeedIndex, with OnRep_IsFlightSprinting.
    bIsFlightSprinting = bSprint;
    Reflection::FParams Params(FindStaticFunction_OnRep_IsFlightSprinting(), true);
    Params.Call(this);
#endif
}

void AFortPlayerControllerAthena::ServerCreativeSetFlightSpeedIndex_ImplementationHK(AFortPlayerControllerAthena* This, int32_t Index)
{
    This->ServerCreativeSetFlightSpeedIndex_Implementation(Index);
}

void AFortPlayerControllerAthena::ServerCreativeSetFlightSprint_ImplementationHK(AFortPlayerControllerAthena* This, bool bSprint)
{
    This->ServerCreativeSetFlightSprint_Implementation(bSprint);
}

void AFortPlayerControllerAthena::ServerClaimPortal_ImplementationHK(AFortPlayerControllerAthena* This)
{
    This->ServerClaimPortal_Implementation();
}

void AFortPlayerControllerAthena::ServerReleasePortal_ImplementationHK(AFortPlayerControllerAthena* This)
{
    This->ServerReleasePortal_Implementation();
}

void AFortPlayerControllerAthena::ServerToggleAutoRestartMinigame_ImplementationHK(AFortPlayerControllerAthena* This)
{
    This->ServerToggleAutoRestartMinigame_Implementation();
}

void AFortPlayerControllerAthena::ServerGiveCreativeItem_ImplementationHK(AFortPlayerControllerAthena* This, const FFortItemEntry& CreativeItem)
{
    This->ServerGiveCreativeItem_Implementation(CreativeItem);
}

void AFortPlayerControllerAthena::ServerReloadCreativePlot_ImplementationHK(AFortPlayerControllerAthena* This)
{
    This->ServerReloadCreativePlot_Implementation();
}

void AFortPlayerControllerAthena::ServerStopSavingCreativePlot_ImplementationHK(AFortPlayerControllerAthena* This)
{
    This->ServerStopSavingCreativePlot_Implementation();
}

void AFortPlayerControllerAthena::ResetMyCurrentCreativePlot_ImplementationHK(AFortPlayerControllerAthena* This)
{
    This->ResetMyCurrentCreativePlot_Implementation();
}

void AFortPlayerControllerAthena::ServerTeleportToPlaygroundIslandDock_ImplementationHK(AFortPlayerControllerAthena* This)
{
    This->ServerTeleportToPlaygroundIslandDock_Implementation();
}

void AFortPlayerControllerAthena::ServerTeleportToPlaygroundLobbyIsland_ImplementationHK(AFortPlayerControllerAthena* This)
{
    This->ServerTeleportToPlaygroundLobbyIsland_Implementation();
}

bool AFortPlayerControllerAthena::ServerLoadPlotForPortal_ValidateHK(AFortPlayerControllerAthena* This, AFortAthenaCreativePortal* Portal, const FString& PlotItemId)
{
    return This->ServerLoadPlotForPortal_Validate(Portal, PlotItemId);
}

void AFortPlayerControllerAthena::ServerLoadPlotForPortal_ImplementationHK(AFortPlayerControllerAthena* This, AFortAthenaCreativePortal* Portal, const FString& PlotItemId)
{
    This->ServerLoadPlotForPortal_Implementation(Portal, PlotItemId);
}

void AFortPlayerControllerAthena::HookCreativeVTables()
{
#if WITH_SERVER_CODE
    // 7.00's Creative (Playlist_PlaygroundV2): the new RPCs' _Implementations are the shared empty function. Older
    // builds have no Creative rift class, and nothing here runs.
    if (!AFortAthenaCreativePortal::StaticClass() || !FindStaticFunction_ServerGiveCreativeItem())
    {
        return;
    }
    RestoreImplementation(FindStaticFunction_ServerGiveCreativeItem(), reinterpret_cast<void*>(&ServerGiveCreativeItem_ImplementationHK), L"ServerGiveCreativeItem");
    RestoreImplementation(FindStaticFunction_ServerReloadCreativePlot(), reinterpret_cast<void*>(&ServerReloadCreativePlot_ImplementationHK), L"ServerReloadCreativePlot");
    RestoreImplementation(FindStaticFunction_ServerStopSavingCreativePlot(), reinterpret_cast<void*>(&ServerStopSavingCreativePlot_ImplementationHK), L"ServerStopSavingCreativePlot");
    RestoreImplementation(FindStaticFunction_ResetMyCurrentCreativePlot(), reinterpret_cast<void*>(&ResetMyCurrentCreativePlot_ImplementationHK), L"ResetMyCurrentCreativePlot");
    RestoreImplementation(FindStaticFunction_ServerTeleportToPlaygroundIslandDock(), reinterpret_cast<void*>(&ServerTeleportToPlaygroundIslandDock_ImplementationHK),
        L"ServerTeleportToPlaygroundIslandDock");
    RestoreImplementation(FindStaticFunction_ServerTeleportToPlaygroundLobbyIsland(), reinterpret_cast<void*>(&ServerTeleportToPlaygroundLobbyIsland_ImplementationHK),
        L"ServerTeleportToPlaygroundLobbyIsland");
    // 7.40 (CL-5046157): the new rift and auto-restart RPCs (slots 0x430, 0x42E, 0x420 hold the shared empty function
    // 0x2B5120). Earlier builds have none of the three UFunctions.
    if (FindStaticFunction_ServerClaimPortal())
    {
        RestoreImplementation(FindStaticFunction_ServerClaimPortal(), reinterpret_cast<void*>(&ServerClaimPortal_ImplementationHK), L"ServerClaimPortal");
    }
    if (FindStaticFunction_ServerReleasePortal())
    {
        RestoreImplementation(FindStaticFunction_ServerReleasePortal(), reinterpret_cast<void*>(&ServerReleasePortal_ImplementationHK), L"ServerReleasePortal");
    }
    if (FindStaticFunction_ServerToggleAutoRestartMinigame())
    {
        RestoreImplementation(FindStaticFunction_ServerToggleAutoRestartMinigame(), reinterpret_cast<void*>(&ServerToggleAutoRestartMinigame_ImplementationHK),
            L"ServerToggleAutoRestartMinigame");
        if (!FindStaticFunction_GetMinigame() || !AFortMinigame::FindStaticFunction_ToggleAutoRestartMinigame())
        {
            Log::Error(L"Finder: AFortPlayerControllerAthena::ServerToggleAutoRestartMinigame's restoration is missing a part (GetMinigame {}, AFortMinigame::ToggleAutoRestartMinigame {}), so the toggle does nothing",
                FindStaticFunction_GetMinigame() ? L"found" : L"missing", AFortMinigame::FindStaticFunction_ToggleAutoRestartMinigame() ? L"found" : L"missing");
        }
    }

    // 9.00 (CL-6337466): the Creative flight speed and sprint RPCs. Earlier builds have neither UFunction.
    if (FindStaticFunction_ServerCreativeSetFlightSpeedIndex() && FindStaticFunction_ServerCreativeSetFlightSprint())
    {
        if (FindStaticFunction_OnRep_FlyingModifierIndex() && FindStaticFunction_OnRep_IsFlightSprinting())
        {
            RestoreImplementation(FindStaticFunction_ServerCreativeSetFlightSpeedIndex(), reinterpret_cast<void*>(&ServerCreativeSetFlightSpeedIndex_ImplementationHK),
                L"ServerCreativeSetFlightSpeedIndex");
            RestoreImplementation(FindStaticFunction_ServerCreativeSetFlightSprint(), reinterpret_cast<void*>(&ServerCreativeSetFlightSprint_ImplementationHK),
                L"ServerCreativeSetFlightSprint");
        }
        else
        {
            Log::Error(L"Finder: AFortPlayerControllerAthena's Creative flight OnReps weren't found (OnRep_FlyingModifierIndex {}, OnRep_IsFlightSprinting {}), so flight speed and sprint stay the client's only",
                FindStaticFunction_OnRep_FlyingModifierIndex() ? L"found" : L"missing", FindStaticFunction_OnRep_IsFlightSprinting() ? L"found" : L"missing");
        }
    }

    // 9.00: the phone's selection and spawning server code (AFortCreativeMoveTool).
    AFortCreativeMoveTool::RestoreStrippedServerFunctions();

    // 9.00 (#658): the phone's grant, which the blueprint left to SetIsCreativeModeEnabled. Earlier builds have no
    // GrantCreativePhoneItemDefLoadedCallback (their blueprint gives the phone).
    if (UFunction* Grant = FindStaticFunction_GrantCreativePhoneItemDefLoadedCallback())
    {
        const uintptr_t Exec = reinterpret_cast<uintptr_t>(Grant->GetNativeFunc());
        // The exec's first call or tail jump (9.00 0xA1E075: jmp to the shared empty function).
        bool bStripped = false;
        for (uintptr_t Site = Exec; Exec && Site < Exec + 0x40; ++Site)
        {
            const uint8_t Opcode = *reinterpret_cast<const uint8_t*>(Site);
            if (Opcode == 0xE8 || Opcode == 0xE9)
            {
                const uintptr_t Target = Finder::RelativeTarget(Site, 1, 5);
                if (Finder::FunctionStart(Target) == Target || Finder::IsStubFunction(Target))
                {
                    bStripped = Finder::IsStubFunction(Target);
                    break;
                }
            }
        }
        UFunction* Signature = FindStaticFunction_OnCreativeModeEnabledChanged();
        if (bStripped && Signature && FindStaticFunction_IsCreativeModeEnabled() && Member_CreativeMoveToolItemDefinition.IsAvailable() && Member_OnCreativeModeEnabledChangedDelegate.IsAvailable())
        {
            Grant->SetNativeFunc(&execGrantCreativePhoneItemDefLoadedCallback);
            CreativeModeEnabledChangedHandler = StaticClass()->AddNativeFunction(L"HandleCreativeModeEnabledChanged", &execHandleCreativeModeEnabledChanged, Signature);
            Log::Info(L"Hook AFortPlayerControllerGameplay::GrantCreativePhoneItemDefLoadedCallback: native (the build's exec 0x{:X} calls only the shared empty function); OnCreativeModeEnabledChangedDelegate {}",
                Exec - Finder::ImageBase(), CreativeModeEnabledChangedHandler ? L"handled" : L"not handled (no native added)");
        }
        else
        {
            Log::Info(L"Finder: AFortPlayerControllerGameplay::GrantCreativePhoneItemDefLoadedCallback is native in this build or its parts are missing (stripped {})", bStripped ? L"yes" : L"no");
        }
    }

    // MoveToolRemoved (#554): only where the quickbars are the client's (7.40), and its exec is the empty one many
    // stripped UFunctions share (7.40 0x866D10). Older builds' server quickbars take the held phone out of the hands.
    if (UFunction* MoveTool = FindStaticFunction_MoveToolRemoved(); MoveTool && !AFortPlayerController::HasQuickBars() && AFortPlayerController::HasDelayedQuickBarActions()
        && Finder::ResolveWeaponPendingDestroyLive(AFortWeapon::PendingDestroyDueToDurabilityOffset(), AFortWeapon::PendingDestroyDueToDurabilityMask()))
    {
        const FNativeFuncPtr Exec = MoveTool->GetNativeFunc();
        int32_t Sharing = 0;
        for (TObjectIterator<UFunction> It; Exec && It && Sharing <= 16; ++It)
        {
            Sharing += (*It)->GetNativeFunc() == Exec;
        }
        if (Sharing > 16)
        {
            MoveTool->SetNativeFunc(&execMoveToolRemoved);
            Log::Info(L"Hook AFortPlayerControllerAthena::MoveToolRemoved: native (the build's exec +0x{:X} is the shared empty one)", reinterpret_cast<uintptr_t>(Exec) - Finder::ImageBase());
        }
        else
        {
            Log::Info(L"Finder: AFortPlayerControllerAthena::MoveToolRemoved is native in this build (exec +0x{:X})", reinterpret_cast<uintptr_t>(Exec) - Finder::ImageBase());
        }
    }

    // ServerLoadPlotForPortal_Implementation (7.00 0xC65E80) is the game's, but for the rift's half of the load: once it
    // has the player's id (0x1285700) it calls the shared empty function with the rift, the id and the plot item
    // (0xC65FEA), then reads the plot item's id through its vtable (call [rax+418h]) to send SetLastUsedCreativePlot.
    UFunction* LoadForPortal = FindStaticFunction_ServerLoadPlotForPortal();
    const int32_t LoadIndex = LoadForPortal ? Finder::ImplementationIndex(LoadForPortal) : -1;
    UObject* Default = LoadIndex >= 0 ? StaticClass()->GetDefaultObject(false) : nullptr;
    const uintptr_t LoadImplementation = Default ? reinterpret_cast<uintptr_t>((*reinterpret_cast<void***>(Default))[LoadIndex]) : 0;
    uint8_t* StrippedCall = nullptr;
    for (const auto& [Site, Target] : LoadImplementation && !Finder::IsStubFunction(LoadImplementation) ? Finder::DecodedDirectCalls(LoadImplementation) : std::vector<std::pair<uintptr_t, uintptr_t>>())
    {
        if (Finder::IsStubFunction(Target))
        {
            StrippedCall = reinterpret_cast<uint8_t*>(Site);
            break;
        }
    }
    if (LoadImplementation && Finder::IsStubFunction(LoadImplementation))
    {
        // 7.20 strips both halves: the exec (0x175C530) calls the return-zero stub as _Validate (slot 0x3FC, call
        // [rax+1FE0h]; test al, al), so every pick at a rift was RPC_ValidateFailed, and the empty one as _Implementation
        // (slot 0x3FD). Both are restored as 7.10 has them (Core's ServerLoadPlotForPortal_Validate/_Implementation).
        void** DefaultVTable = *reinterpret_cast<void***>(Default);
        const int32_t ValidateIndex = Finder::ValidateIndex(LoadForPortal);
        void* const ValidateStub = ValidateIndex >= 0 && ValidateIndex < LoadIndex ? DefaultVTable[ValidateIndex] : nullptr;
        const bool bValidateStripped = ValidateStub && Finder::IsStubFunction(reinterpret_cast<uintptr_t>(ValidateStub));
        if (bValidateStripped)
        {
            SwapStubInSubclasses(ValidateIndex, ValidateStub, reinterpret_cast<void*>(&ServerLoadPlotForPortal_ValidateHK),
                L"AFortPlayerControllerAthena::ServerLoadPlotForPortal_Validate");
        }
        RestoreImplementation(LoadForPortal, reinterpret_cast<void*>(&ServerLoadPlotForPortal_ImplementationHK), L"ServerLoadPlotForPortal");
        bServerLoadPlotForPortalRestored = true;
        Log::Info(L"Finder: AFortPlayerControllerAthena::ServerLoadPlotForPortal is stripped (_Validate slot 0x{:X} {}, _Implementation slot 0x{:X}): restored; "
            L"ABuildingActor::GetOwningPlayerState {}, UFortMcpProfileCreative::SetLastUsedCreativePlot {}, the plot item's id {}",
            ValidateIndex, !ValidateStub ? L"not found" : bValidateStripped ? L"a stub" : L"native", LoadIndex,
            ABuildingActor::FindStaticFunction_GetOwningPlayerState() ? L"found" : L"missing",
            UFortMcpProfileCreative::FindStaticFunction_SetLastUsedCreativePlot() ? L"found" : L"missing",
            Natives::UMcpItem_GetItemIdIndex >= 0 ? std::format(L"slot 0x{:X}", Natives::UMcpItem_GetItemIdIndex) : std::wstring(L"UFortAccountItem::InstanceId"));
        if (!ValidateStub || !ABuildingActor::FindStaticFunction_GetOwningPlayerState() || !UFortMcpProfileCreative::FindStaticFunction_SetLastUsedCreativePlot())
        {
            Log::Error(L"Finder: AFortPlayerControllerAthena::ServerLoadPlotForPortal's restoration is missing a part, so a pick at a rift {} (see the line above)",
                !ValidateStub ? L"is refused and disconnects the player" : L"doesn't check the rift's owner or save the last used island");
        }
    }
    else if (StrippedCall)
    {
        Natives::AFortPlayerControllerAthena_ServerLoadPlotForPortal = reinterpret_cast<decltype(Natives::AFortPlayerControllerAthena_ServerLoadPlotForPortal)>(LoadImplementation);
        // The id getter: the first `call qword ptr [rax+disp32]` after the stripped call.
        for (const uint8_t* Code = StrippedCall + 5; Code < StrippedCall + 0x30 && Finder::IsReadable(Code, 6); ++Code)
        {
            if (Code[0] == 0xFF && Code[1] == 0x90)
            {
                const int32_t Displacement = *reinterpret_cast<const int32_t*>(Code + 2);
                Natives::UMcpItem_GetItemIdIndex = Displacement > 0 && Displacement % 8 == 0 ? Displacement / 8 : -1;
                break;
            }
        }
        Hooks::RedirectCall(L"AFortPlayerControllerAthena::ServerLoadPlotForPortal_Implementation's AFortAthenaCreativePortal::LoadPlotForUser", StrippedCall,
            reinterpret_cast<void*>(&AFortAthenaCreativePortal::LoadPlotForUserHK));
        Log::Info(L"Finder: AFortPlayerControllerAthena::ServerLoadPlotForPortal_Implementation at 0x{:X}, its stripped rift call at 0x{:X}, the plot item's id getter slot 0x{:X}",
            LoadImplementation - Finder::ImageBase(), reinterpret_cast<uintptr_t>(StrippedCall) - Finder::ImageBase(), Natives::UMcpItem_GetItemIdIndex);
    }
    else
    {
        Log::Error(L"Finder: AFortPlayerControllerAthena::ServerLoadPlotForPortal_Implementation's stripped rift call not found, so no island loads");
    }

    // The island link's setter: the function that binds &AFortPlayerControllerAthena::OnMinigameVolumeStateChanged (one on
    // 7.00, 7.10 and 7.20). Its last loop refreshes the old island's rifts: the one of its calls whose body clears the
    // rift's CurrentPopulation (mov byte ptr [reg+CurrentPopulation], 0) before counting it again.
    const std::set<uintptr_t> LinkSetters = FunctionsReferencing(L"&AFortPlayerControllerAthena::OnMinigameVolumeStateChanged");
    const int32_t PopulationOffset = AFortAthenaCreativePortal::CurrentPopulationOffset();
    if (LinkSetters.size() == 1 && PopulationOffset > 0)
    {
        const uintptr_t Setter = *LinkSetters.begin();
        for (const auto& [Site, Target] : Finder::DecodedDirectCalls(Setter))
        {
            const auto* Code = reinterpret_cast<const uint8_t*>(Target);
            bool bClearsPopulation = false;
            for (int32_t At = 0; At + 7 <= 0x800 && !bClearsPopulation && Finder::IsReadable(Code + At, 7); ++At)
            {
                bClearsPopulation = Code[At] == 0xC6 && (Code[At + 1] & 0xF8) == 0x80 && (Code[At + 1] & 0x07) != 0x04
                    && *reinterpret_cast<const int32_t*>(Code + At + 2) == PopulationOffset && Code[At + 6] == 0x00;
            }
            if (bClearsPopulation)
            {
                Natives::AFortPlayerControllerAthena_SetCreativePlotLinkedVolume = reinterpret_cast<decltype(Natives::AFortPlayerControllerAthena_SetCreativePlotLinkedVolume)>(Setter);
                Natives::AFortAthenaCreativePortal_RefreshPortal = reinterpret_cast<decltype(Natives::AFortAthenaCreativePortal_RefreshPortal)>(Target);
                break;
            }
        }
    }
    if (Natives::AFortPlayerControllerAthena_SetCreativePlotLinkedVolume)
    {
        Log::Info(L"Finder: AFortPlayerControllerAthena's CreativePlotLinkedVolume setter 0x{:X}, the rift refresh 0x{:X} (CurrentPopulation +0x{:X})",
            reinterpret_cast<uintptr_t>(Natives::AFortPlayerControllerAthena_SetCreativePlotLinkedVolume) - Finder::ImageBase(),
            reinterpret_cast<uintptr_t>(Natives::AFortAthenaCreativePortal_RefreshPortal) - Finder::ImageBase(), PopulationOffset);
    }
    else
    {
        Log::Error(L"Finder: AFortPlayerControllerAthena's CreativePlotLinkedVolume setter not found ({} candidate(s), CurrentPopulation +0x{:X}), so the rifts don't count who is on their island",
            LinkSetters.size(), PopulationOffset);
    }

    // The island's record from its file's bytes: ULevelSaveRecord::ReadFromBuffer, the call ReadFromDisk (the function
    // that logs "ULevelSaveRecord::ReadFromDisk Invalid File") makes whose own calls include ReadFromArchive (the one
    // that logs "ULevelSaveRecord::ReadFromArchive Failed to decompress data").
    const uintptr_t ReadFromDisk = FunctionReferencing(L"ULevelSaveRecord::ReadFromDisk Invalid File");
    const uintptr_t ReadFromArchive = FunctionReferencing(L"ULevelSaveRecord::ReadFromArchive Failed to decompress data");
    for (const auto& [Site, Target] : ReadFromDisk && ReadFromArchive ? Finder::DecodedDirectCalls(ReadFromDisk) : std::vector<std::pair<uintptr_t, uintptr_t>>())
    {
        const std::vector<std::pair<uintptr_t, uintptr_t>> Inner = Finder::DecodedDirectCalls(Target);
        if (std::any_of(Inner.begin(), Inner.end(), [ReadFromArchive](const auto& Call) { return Call.second == ReadFromArchive; }))
        {
            Natives::ULevelSaveRecord_ReadFromBuffer = reinterpret_cast<decltype(Natives::ULevelSaveRecord_ReadFromBuffer)>(Target);
            break;
        }
    }
    // 8.20 (CL-5625478) drops ReadFromDisk's log line; ReadFromBuffer (0x1338F70) is still ReadFromArchive's only caller,
    // with 8.00's body (a memory reader over the bytes, then ReadFromArchive).
    const std::vector<uintptr_t> ArchiveCalls = !ReadFromDisk && ReadFromArchive ? Finder::CallsTo(ReadFromArchive, false) : std::vector<uintptr_t>();
    if (!Natives::ULevelSaveRecord_ReadFromBuffer && ArchiveCalls.size() == 1)
    {
        Natives::ULevelSaveRecord_ReadFromBuffer = reinterpret_cast<decltype(Natives::ULevelSaveRecord_ReadFromBuffer)>(Finder::FunctionStart(ArchiveCalls.front()));
    }

    // The island's cloud save (its upload, or with bCheckin its checkin) and the sidecar's meta setter, which the save
    // calls once for each of its four meta keys and which is the one of them that formats the file's "%s/%s" key.
    const uintptr_t CloudSave = FunctionReferencing(L"Unable to start cloud save. Authority=%s, Busy=%s");
    if (CloudSave)
    {
        Natives::UFortLevelSaveComponent_CloudSave = reinterpret_cast<decltype(Natives::UFortLevelSaveComponent_CloudSave)>(CloudSave);
        // 7.00 (0xF5ADB0) takes (Plot, UserId, bCheckin) and keeps bCheckin from r9b (movzx edi, r9b); 7.10 (CL-4667333,
        // 0xF88760) reads the plot and its owner from the component and takes bCheckin second (movzx r13d, dl). The first
        // movzx of an argument byte in its prologue tells which.
        const auto* Code = reinterpret_cast<const uint8_t*>(CloudSave);
        bool bArgumentByteFound = false;
        for (int32_t At = 1; At + 3 <= 0x40; ++At)
        {
            if (Code[At] != 0x0F || Code[At + 1] != 0xB6 || (Code[At + 2] & 0xC0) != 0xC0)
            {
                continue;
            }
            const uint8_t Rex = (Code[At - 1] & 0xF0) == 0x40 ? Code[At - 1] : 0;
            const int32_t Source = (Code[At + 2] & 0x07) | ((Rex & 0x01) << 3);
            if (Source == 2 || Source == 9)
            {
                Natives::UFortLevelSaveComponent_CloudSaveTakesCheckinOnly = Source == 2;
                bArgumentByteFound = true;
                break;
            }
        }
        // Neither: 7.30 keeps its ESaveOperation from edx (mov r13d, edx), 7.40+ from r8d (mov r12d, r8d) with the
        // buffer in rdx. The 32-bit mov (no REX.W) of an argument register into another tells which.
        for (int32_t At = 1; !bArgumentByteFound && At + 2 <= 0x40; ++At)
        {
            const uint8_t Rex = (Code[At - 1] & 0xF0) == 0x40 ? Code[At - 1] : 0;
            if (Code[At] != 0x8B || (Code[At + 1] & 0xC0) != 0xC0 || (Rex & 0x08))
            {
                continue;
            }
            const int32_t Source = (Code[At + 1] & 0x07) | ((Rex & 0x01) << 3);
            if (Source == 2 || Source == 8)
            {
                Natives::UFortLevelSaveComponent_CloudSaveTakesOperation = Source == 2;
                Natives::UFortLevelSaveComponent_CloudSaveTakesData = Source == 8;
                break;
            }
        }
        // 7.40+: the record's writer is the call just before the save in its callers (8.20: 0x11EDBAF in the periodic
        // save, 0x11D5001 in EndPlay, both 0x133EC80).
        if (Natives::UFortLevelSaveComponent_CloudSaveTakesData)
        {
            TMap<uintptr_t, int32_t> Writers;
            for (const uintptr_t Site : Finder::CallsTo(CloudSave, false))
            {
                const uintptr_t Caller = Finder::FunctionStart(Site);
                uintptr_t Previous = 0;
                uintptr_t PreviousSite = 0;
                for (const auto& [CallSite, Target] : Caller ? Finder::DecodedDirectCalls(Caller) : std::vector<std::pair<uintptr_t, uintptr_t>>())
                {
                    if (CallSite < Site && CallSite > PreviousSite)
                    {
                        PreviousSite = CallSite;
                        Previous = Target;
                    }
                }
                if (Previous && Site - PreviousSite <= 0x40)
                {
                    ++Writers.FindOrAdd(Previous);
                }
            }
            for (const auto& Pair : Writers)
            {
                if (Pair.Value >= 2)
                {
                    Natives::ULevelSaveBaseComponent_SaveLevelRecordToBuffer = reinterpret_cast<decltype(Natives::ULevelSaveBaseComponent_SaveLevelRecordToBuffer)>(Pair.Key);
                    break;
                }
            }
        }
        std::set<uintptr_t> KeyFormatters;
        for (const uintptr_t Ref : Finder::StringRefs(L"%s/%s"))
        {
            KeyFormatters.insert(Finder::FunctionStart(Ref));
        }
        std::map<uintptr_t, int32_t> Counts;
        for (const auto& [Site, Target] : Finder::DecodedDirectCalls(CloudSave))
        {
            ++Counts[Target];
        }
        for (const auto& [Target, Count] : Counts)
        {
            if (Count >= 4 && KeyFormatters.contains(Target))
            {
                Natives::USidecarSys_SetMeta = reinterpret_cast<decltype(Natives::USidecarSys_SetMeta)>(Target);
                break;
            }
        }
        // 7.40 (CL-5046157) sets the four meta keys in a function of their own, which the save calls (0x10BE800 calls
        // 0x10BE030 at 0x10BEABA, which calls the setter 0x2FABCA0 four times): read the save's callees the same way.
        for (const auto& [Callee, CalleeCount] : Natives::USidecarSys_SetMeta ? std::map<uintptr_t, int32_t>{} : Counts)
        {
            std::map<uintptr_t, int32_t> InnerCounts;
            for (const auto& [Site, Target] : Finder::FunctionStart(Callee) == Callee ? Finder::DecodedDirectCalls(Callee) : std::vector<std::pair<uintptr_t, uintptr_t>>())
            {
                ++InnerCounts[Target];
            }
            for (const auto& [Target, Count] : InnerCounts)
            {
                if (Count >= 4 && KeyFormatters.contains(Target) && !Natives::USidecarSys_SetMeta)
                {
                    Natives::USidecarSys_SetMeta = reinterpret_cast<decltype(Natives::USidecarSys_SetMeta)>(Target);
                }
            }
            if (Natives::USidecarSys_SetMeta)
            {
                break;
            }
        }
    }
    // The island load's queue and steps (after ReadFromBuffer, which tells ULevelSaveRecord's end of a read from ReadFromDisk).
    ResolveIslandLoad();

    // The creative profile, which LoadCreativePlots reads (McpProfileSys, intact). UMcpProfile::GetStat: the second call
    // after the creative profile's plot permissions getter (0x1132C70) names "creative_plot_permissions" (the first is
    // the name's copy into its FString), the one whose answer's EJson type it checks next (cmp dword ptr [reg+8], 6).
    // 7.30 (CL-4834550) has no "creative_plot_permissions" left; the daily rewards reader asks GetStat the same way
    // (0xFBAAC0: the call at 0xFBAB48 to GetStat 0xA4D4F0, then cmp dword ptr [rcx+8], 6; 7.20 0xF96F60 / 0xA3A7D0), so
    // its "daily_rewards" is tried when the old string is gone.
    std::vector<uintptr_t> StatRefs = Finder::StringRefs(L"creative_plot_permissions");
    if (StatRefs.empty())
    {
        StatRefs = Finder::StringRefs(L"daily_rewards");
    }
    for (const uintptr_t Ref : StatRefs)
    {
        const uintptr_t Function = Finder::FunctionStart(Ref);
        int32_t CallsAfter = 0;
        for (const auto& [Site, Target] : Function ? Finder::DecodedDirectCalls(Function) : std::vector<std::pair<uintptr_t, uintptr_t>>())
        {
            if (Site <= Ref || ++CallsAfter < 2)
            {
                continue;
            }
            for (const uint8_t* Code = reinterpret_cast<const uint8_t*>(Site + 5); Code < reinterpret_cast<const uint8_t*>(Site + 0x30) && Finder::IsReadable(Code, 4); ++Code)
            {
                if (Code[0] == 0x83 && (Code[1] & 0xF8) == 0x78 && (Code[1] & 7) != 4 && Code[2] == 0x08 && Code[3] == 0x06)
                {
                    Natives::UMcpProfile_GetStat = reinterpret_cast<decltype(Natives::UMcpProfile_GetStat)>(Target);
                    break;
                }
            }
            break;
        }
        if (Natives::UMcpProfile_GetStat)
        {
            break;
        }
    }
    // UMcpProfile's ItemsUpdatedDelegate: the member HandleProfileUpdate (0x955BE0, the function that logs its lock
    // expiration) broadcasts last, after the changed items' own delegates (lea rcx, [r15+0F8h]; call 0x2E4F40), inside
    // the profile's reflected size.
    const uintptr_t HandleProfileUpdate = FunctionReferencing(L"UMcpProfile::HandleProfileUpdate updated lock expiration on Account %s for ProfileId %s to %s");
    const std::vector<std::pair<ptrdiff_t, uintptr_t>> ProfileMemberCalls = HandleProfileUpdate ? Finder::MemberThisCalls(HandleProfileUpdate) : std::vector<std::pair<ptrdiff_t, uintptr_t>>();
    UClass* ProfileClass = UMcpProfile::StaticClass();
    const ptrdiff_t ProfileSize = ProfileClass ? ProfileClass->GetPropertiesSize() : 0;
    if (!ProfileMemberCalls.empty() && ProfileMemberCalls.back().first >= 0x28 && ProfileMemberCalls.back().first + 0x10 <= ProfileSize)
    {
        Natives::UMcpProfile_ItemsUpdatedDelegate = ProfileMemberCalls.back().first;
    }
    else if (HandleProfileUpdate)
    {
        // 8.20: the last call after mov rcx, [reg+disp32] (0xBC01B4: 48 8B 8B E8 00 00 00, then call 0xBB3030), whose
        // callee ends add rcx, imm32 ... jmp (0xBB32C6: 48 81 C1 08 01 00 00, the broadcast).
        for (const auto& [Site, Target] : Finder::DecodedDirectCalls(HandleProfileUpdate))
        {
            const uint8_t* Load = reinterpret_cast<const uint8_t*>(Site - 7);
            if (!Finder::IsReadable(Load, 7) || Load[0] != 0x48 || Load[1] != 0x8B || (Load[2] & 0xF8) != 0x88 || (Load[2] & 7) == 4)
            {
                continue;
            }
            const ptrdiff_t Holder = *reinterpret_cast<const int32_t*>(Load + 3);
            const uintptr_t End = Finder::FunctionEnd(Target);
            ptrdiff_t Inner = -1;
            for (uintptr_t At = Target; End && At + 12 <= End; ++At)
            {
                const uint8_t* Code = reinterpret_cast<const uint8_t*>(At);
                if (Code[0] != 0x48 || Code[1] != 0x81 || Code[2] != 0xC1)
                {
                    continue;
                }
                for (const uint8_t* Next = Code + 7; Next < Code + 0x30 && reinterpret_cast<uintptr_t>(Next) < End; ++Next)
                {
                    if (*Next == 0xE9)
                    {
                        Inner = *reinterpret_cast<const int32_t*>(Code + 3);
                        break;
                    }
                }
            }
            if (Holder >= 0x28 && Holder + 8 <= ProfileSize && Inner > 0 && Inner < 0x1000)
            {
                Natives::UMcpProfile_ItemsUpdatedDelegateHolder = Holder;
                Natives::UMcpProfile_ItemsUpdatedDelegate = Inner;
            }
        }
    }
    Log::Info(L"Finder: Creative's profile: UMcpProfile::GetStat 0x{:X}, UMcpProfile::ItemsUpdatedDelegate {} (HandleProfileUpdate 0x{:X}, {} member call(s))",
        Natives::UMcpProfile_GetStat ? reinterpret_cast<uintptr_t>(Natives::UMcpProfile_GetStat) - Finder::ImageBase() : 0,
        Natives::UMcpProfile_ItemsUpdatedDelegateHolder >= 0
            ? std::format(L"+0x{:X} of [profile+0x{:X}]", Natives::UMcpProfile_ItemsUpdatedDelegate, Natives::UMcpProfile_ItemsUpdatedDelegateHolder)
            : std::format(L"+0x{:X}", Natives::UMcpProfile_ItemsUpdatedDelegate),
        HandleProfileUpdate ? HandleProfileUpdate - Finder::ImageBase() : 0, ProfileMemberCalls.size());
    if (!Natives::UMcpProfile_GetStat || Natives::UMcpProfile_ItemsUpdatedDelegate <= 0)
    {
        Log::Error(L"Finder: Creative's profile reader is missing a part, so players' last used islands or their late profiles aren't read (see the line above)");
    }

    const auto Address = [](const void* Function) { return Function ? reinterpret_cast<uintptr_t>(Function) - Finder::ImageBase() : 0; };
    const std::wstring CloudSaveShape = Natives::UFortLevelSaveComponent_CloudSaveTakesData
        ? std::format(L"takes the written record and its ESaveOperation; the record's writer 0x{:X}", Address(reinterpret_cast<const void*>(Natives::ULevelSaveBaseComponent_SaveLevelRecordToBuffer)))
        : Natives::UFortLevelSaveComponent_CloudSaveTakesOperation ? L"takes its ESaveOperation"
        : Natives::UFortLevelSaveComponent_CloudSaveTakesCheckinOnly ? L"takes bCheckin only" : L"takes the plot, its owner and bCheckin";
    Log::Info(L"Finder: Creative's island record: ULevelSaveRecord::ReadFromBuffer 0x{:X}, UFortLevelSaveComponent's cloud save 0x{:X} ({}), USidecarSys's meta setter 0x{:X}",
        Address(reinterpret_cast<const void*>(Natives::ULevelSaveRecord_ReadFromBuffer)), Address(reinterpret_cast<const void*>(Natives::UFortLevelSaveComponent_CloudSave)),
        CloudSaveShape, Address(reinterpret_cast<const void*>(Natives::USidecarSys_SetMeta)));
    if (!Natives::ULevelSaveRecord_ReadFromBuffer || !Natives::UFortLevelSaveComponent_CloudSave || !Natives::USidecarSys_SetMeta
        || (Natives::UFortLevelSaveComponent_CloudSaveTakesData && !Natives::ULevelSaveBaseComponent_SaveLevelRecordToBuffer))
    {
        Log::Error(L"Finder: Creative's island record is missing a native, so islands load or save incompletely (see the line above)");
    }
#endif
}
