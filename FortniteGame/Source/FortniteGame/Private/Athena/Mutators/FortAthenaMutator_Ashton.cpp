#include "pch.h"
#include "Athena/Mutators/FortAthenaMutator_Ashton.h"
#include "Athena/FortGameStateAthena.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "Athena/FortPlayerStateAthena.h"
#include "Athena/FortPlaylistAthena.h"
#include "Engine/World.h"
#include "Finder.h"
#include "Natives.h"
#include "FortGameModeZone.h"
#include "FortKismetLibrary.h"
#include "Hooks.h"
#include "Items/FortPickup.h"
#include "Items/FortInventoryOwnerInterface.h"
#include "Items/FortItemDefinition.h"
#include "Log.h"
#include "Logging/LogMacros.h"
#include "ObjectLayout.h"
#include "Templates/Casts.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogAshtonMutator, Log, All);

AFortAthenaMutator_Ashton* AFortAthenaMutator_Ashton::GetCurrent(UWorld* World)
{
    AFortGameStateAthena* GameState = World && StaticClass() ? Cast<AFortGameStateAthena>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    const TArray<UObject*>* MutatorList = GameState ? GameState->GetGameplayMutatorList() : nullptr;
    if (!MutatorList)
    {
        return nullptr;
    }
    const TArray<UObject*>& Mutators = *MutatorList;
    for (int32_t Index = 0; Index < Mutators.Num(); ++Index)
    {
        if (AFortAthenaMutator_Ashton* Ashton = Cast<AFortAthenaMutator_Ashton>(Mutators[Index]))
        {
            return Ashton;
        }
    }
    return nullptr;
}

#if WITH_SERVER_CODE
uint8_t AFortAthenaMutator_Ashton::AFortGameModeAthena_OnPawnDeathBeforeRespawning(AFortPlayerPawnAthena* Pawn)
{
    const int32_t Index = Natives::AFortAthenaMutator_Ashton_OnPawnDeathBeforeRespawningIndex;
    if (Pawn && Index >= 0)
    {
        return CallVirtual<uint8_t>(this, Index, Pawn);
    }
    return 0; // EFortMutatorReturnValue::Ignore
}
#endif

uint8_t AFortAthenaMutator_Ashton::GetVillainTeamNum() const
{
    const ptrdiff_t Offset = Member_OnVillainRespawnsRemainingChanged.GetOffsetChecked() + static_cast<ptrdiff_t>(sizeof(FMulticastScriptDelegate));
    return *(reinterpret_cast<const uint8_t*>(this) + Offset);
}

bool AFortAthenaMutator_Ashton::IsVillain(const AFortPlayerStateAthena* PlayerState) const
{
    UWorld* World = PlayerState ? GetWorld() : nullptr;
    AFortGameStateAthena* GameState = World ? Cast<AFortGameStateAthena>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    const UFortPlaylistAthena* Playlist = GameState && AFortGameStateAthena::HasCurrentPlaylist() ? GameState->GetCurrentPlaylist() : nullptr;
    if (!Playlist || !UFortPlaylist::HasDefaultFirstTeam())
    {
        return false;
    }
    const int32_t Team = static_cast<uint8_t>(PlayerState->TeamIndex);
    return Team - static_cast<int32_t>(Playlist->DefaultFirstTeam) == static_cast<int32_t>(GetVillainTeamNum());
}

bool AFortAthenaMutator_Ashton::CanInteractWithGameObject(AFortPickup* Pickup, AFortPlayerController* Controller) const
{
    static UClass* InteractInterface = FindObject<UClass>(nullptr, L"/Script/FortniteGame.FortInteractInterface");
    if (!Pickup || !Controller || !InteractInterface || !Natives::UObject_GetInterfaceAddress
        || !Finder::ResolveAshtonCanInteractWithGameObjectLive(Member_OnVillainRespawnsRemainingChanged.GetOffsetChecked() + static_cast<ptrdiff_t>(sizeof(FMulticastScriptDelegate))))
    {
        return false;
    }
    void* Interactable = Natives::UObject_GetInterfaceAddress(reinterpret_cast<UObject*>(Pickup), InteractInterface);
    if (!Interactable)
    {
        return false;
    }
    bool bResult = false;
    using FCanInteractWithGameObject = char (*)(const AFortAthenaMutator_Ashton* This, bool* bOutResult, void* Interactable, AFortPlayerController* Controller);
    const auto Function = reinterpret_cast<FCanInteractWithGameObject>((*reinterpret_cast<void* const* const*>(this))[Natives::AFortAthenaMutator_Ashton_CanInteractWithGameObjectIndex]);
    return Function(this, &bResult, Interactable, Controller) == 2 && bResult;
}

void AFortAthenaMutator_Ashton::GiveVillainLeaderItemCall(AFortPlayerControllerAthena* NewVillainLeader)
{
#if WITH_SERVER_CODE
    AFortAthenaMutator_Ashton* Mutator = NewVillainLeader ? GetCurrent(NewVillainLeader->GetWorld()) : nullptr;
    if (!Mutator || static_cast<AFortPlayerControllerAthena*>(Mutator->VillainLeaderPC) != NewVillainLeader)
    {
        return;
    }
    // 🟡 Core's (inferred): the new leader gives up the Chitauri kit first, VillainItemDefs one each and forced, as the
    // death handler takes it from a villain (0x106F60C). Its AGID_AshtonPack_Milo has the same GadgetPriority as the
    // gauntlet and was given first, so AFortPlayerState's gadget part gathering (slot 0xE7, 0x176D370, which keeps the
    // first gadget's part of each type at an equal priority) kept the Chitauri body and head over the gauntlet's XL
    // Thanos: the player had Thanos's abilities and anim Blueprint in a Chitauri's body until the kit went (CoreV2 #640).
    const TArray<UFortWorldItemDefinition*>& VillainItems = Mutator->VillainItemDefs;
    for (int32_t Index = 0; Index < VillainItems.Num(); ++Index)
    {
        UFortKismetLibrary::K2_RemoveItemFromPlayer(NewVillainLeader, VillainItems[Index], 1, true);
    }
    UFortWorldItemDefinition* LeaderItem = static_cast<UFortGadgetItemDefinition*>(Mutator->VillainLeaderItemDef);
    // Core's: bNotifyPlayer false (the build's FFortItemEntry(VillainLeaderItemDef, 1) says nothing of it).
    UFortKismetLibrary::K2_GiveItemToPlayer(NewVillainLeader, LeaderItem, 1, false);
    UE_LOG(LogAshtonMutator, Log, TEXT("PromoteVillainLeader: %s is Thanos, given %s"), *NewVillainLeader->GetName(), *GetNameSafe(LeaderItem));
#endif
}

int32_t AFortAthenaMutator_Ashton::FindStoneIndex(const UFortWorldItemDefinition* ItemDefinition) const
{
    if (!ItemDefinition || !UFortWorldItemDefinition::HasSpecialActorPickupTag() || !FAshtonStoneData::StaticStruct())
    {
        return -1;
    }
    // The stone whose GameplayTag the pickup's tag is under (SpecialActor.Ashton.Object.P.Pickup under .Object.P), as
    // OnPickupDestroying's lookup (0x1067230) matches them.
    const std::wstring PickupTag = static_cast<FGameplayTag>(const_cast<UFortWorldItemDefinition*>(ItemDefinition)->SpecialActorPickupTag).TagName.ToString().ToStdString();
    const int32_t DataSize = FAshtonStoneData::StaticStruct()->GetStructureSize();
    const uint8_t* DataList = reinterpret_cast<const uint8_t*>(&static_cast<FAshtonStoneData&>(StoneDataList));
    for (int32_t Index = 0; !PickupTag.empty() && DataSize > 0 && Index < 6; ++Index)
    {
        const FAshtonStoneData& Data = *reinterpret_cast<const FAshtonStoneData*>(DataList + static_cast<ptrdiff_t>(Index) * DataSize);
        const std::wstring StoneTag = static_cast<FGameplayTag>(const_cast<FAshtonStoneData&>(Data).GameplayTag).TagName.ToString().ToStdString();
        if (!StoneTag.empty() && (PickupTag == StoneTag || PickupTag.starts_with(StoneTag + L".")))
        {
            return Index;
        }
    }
    return -1;
}

bool AFortAthenaMutator_Ashton::IsStoneCaptured(int32_t StoneIndex) const
{
    const TStructArray<FAshtonStoneState>& Stones = StoneList;
    return StoneIndex >= 0 && StoneIndex < Stones.Num() && Stones[StoneIndex].StoneState == 2;
}

void AFortAthenaMutator_Ashton::OnPickupSpawnedAndReady(AFortPickup* PickupObj, const UFortWorldItemDefinition* PickupItemDef)
{
#if WITH_SERVER_CODE
    if (Role != ROLE_Authority || !PickupItemDef || !UFortWorldItemDefinition::HasSpecialActorPickupTag())
    {
        return;
    }
    const int32_t Found = FindStoneIndex(PickupItemDef);
    TStructArray<FAshtonStoneState>& Stones = StoneList;
    if (Found < 0 || Found >= Stones.Num())
    {
        return;
    }
    FAshtonStoneState& Stone = Stones[Found];
    if (Stone.StoneState != 2)
    {
        Stone.StoneState = 1;
        Stone.bHasEverSpawned = true;
        ForceNetUpdate();
    }
    UE_LOG(LogAshtonMutator, Log, TEXT("OnPickupSpawnedAndReady: %s is stone %d (state %d)"), *GetNameSafe(reinterpret_cast<UObject*>(PickupObj)), Found, static_cast<int32_t>(Stone.StoneState));
#endif
}

void AFortAthenaMutator_Ashton::OnPickupSpawnedAndReadyHK(AFortAthenaMutator_Ashton* This, AFortPickup* PickupObj, const UFortWorldItemDefinition* PickupItemDef)
{
    if (This)
    {
        This->OnPickupSpawnedAndReady(PickupObj, PickupItemDef);
    }
}

DEFINE_FUNCTION(AFortAthenaMutator_Ashton::execOnPickupSpawnedAndReady)
{
    P_GET_OBJECT(AFortPickup, Z_Param_PickupObj);
    P_GET_OBJECT(UFortWorldItemDefinition, Z_Param_PickupItemDef);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortAthenaMutator_Ashton*>(Context)->OnPickupSpawnedAndReady(Z_Param_PickupObj, Z_Param_PickupItemDef);
    P_NATIVE_END;
}

void AFortAthenaMutator_Ashton::HookNatives()
{
#if WITH_SERVER_CODE
    if (!StaticClass())
    {
        return;
    }
    if (!HasVillainMembers())
    {
        Log::Info(L"Endgame: the mutator's members aren't all in this build, nothing is restored");
        return;
    }

    // The stone capture: the pickup's give tells its owner (AFortPickup::GivePickupTo).
    const bool bPickupGiven = Finder::ResolvePickupGivenLive();
    // The team loadouts: what the game mode's GetStartingItems adds for AshtonInventoryMutator (AFortGameModeAthena's
    // SpawnDefaultPawnFor_Implementation gives it at the aircraft's exit).
    const bool bStartingItems = Finder::ResolveGetStartingItemsLive(AFortGameModeZone::StartingItemsOffset());

    // Thanos's stone pickups: the mutator's answer that lets a villain interact with a stone (Core's ServerHandlePickup).
    Finder::ResolveAshtonCanInteractWithGameObjectLive(Member_OnVillainRespawnsRemainingChanged.GetOffsetChecked() + static_cast<ptrdiff_t>(sizeof(FMulticastScriptDelegate)));
    // The win (MutatorControlled), which Core's bots' removal asks as RemoveFromAlivePlayers does a player's.
    Finder::ResolveCheckWinConditionOnPlayerRemovalLive();

    Finder::FAshtonMutatorSites Sites;
    const ptrdiff_t RespawnsRemaining = Member_NumVillainRespawnsRemaining.IsAvailable() ? Member_NumVillainRespawnsRemaining.GetOffsetChecked() : -1;
    if (!Finder::ResolveAshtonMutatorLive(Member_VillainLeaderPC.GetOffsetChecked(), Member_VillainLeaderItemDef.GetOffsetChecked(), Member_VillainItemDefs.GetOffsetChecked(), RespawnsRemaining, Sites))
    {
        Log::Info(L"Endgame: the gauntlet's give and the death removals are not restored; stone capture {}, team loadouts {}", bPickupGiven ? L"on" : L"off", bStartingItems ? L"on" : L"off");
        return;
    }
    // 9.20 (CL-6822798) binds the handler by name only, with no lea of its address (BeginPlay 0x11864C0): the exec
    // (0x1B46790) calling the stub (0x352C30) shows it stripped.
    UFunction* PickupSpawnedAndReady = FindObject<UFunction>(nullptr, L"/Script/FortniteGame.FortAthenaMutator_Ashton:OnPickupSpawnedAndReady");
    bool bPickupSpawnedAndReadyStripped = Sites.PickupSpawnedAndReadyLea != nullptr;
    for (const auto& [Site, Target] : !bPickupSpawnedAndReadyStripped && PickupSpawnedAndReady && PickupSpawnedAndReady->GetNativeFunc()
        ? Finder::DecodedDirectCalls(reinterpret_cast<uintptr_t>(PickupSpawnedAndReady->GetNativeFunc())) : std::vector<std::pair<uintptr_t, uintptr_t>>{})
    {
        bPickupSpawnedAndReadyStripped = bPickupSpawnedAndReadyStripped || Finder::IsStubFunction(Target);
    }
    if (bPickupSpawnedAndReadyStripped && FAshtonStoneState::StaticStruct() && FAshtonStoneData::StaticStruct() && Member_StoneList.IsAvailable() && Member_StoneDataList.IsAvailable())
    {
        if (Sites.PickupSpawnedAndReadyLea)
        {
            Hooks::RedirectAddress(L"AFortAthenaMutator_Ashton::BeginPlay's OnPickupSpawnedAndReady binding", Sites.PickupSpawnedAndReadyLea, reinterpret_cast<void*>(&OnPickupSpawnedAndReadyHK));
        }
        if (PickupSpawnedAndReady)
        {
            PickupSpawnedAndReady->SetNativeFunc(&execOnPickupSpawnedAndReady);
            Log::Info(L"Endgame: AFortAthenaMutator_Ashton::OnPickupSpawnedAndReady is stripped, restored{}", Sites.PickupSpawnedAndReadyLea ? L" (and its binding's address)" : L" (bound by name)");
        }
    }
    Hooks::RedirectCall(L"AFortAthenaMutator_Ashton promotion -> UFortKismetLibrary::K2_GiveItemToPlayer (VillainLeaderItemDef)", Sites.LeaderGiveCall, reinterpret_cast<void*>(&GiveVillainLeaderItemCall));
    Hooks::RedirectCall(L"AFortAthenaMutator_Ashton villain leader death -> UFortKismetLibrary::K2_RemoveItemFromPlayer", Sites.LeaderRemoveCall, reinterpret_cast<void*>(&UFortKismetLibrary::K2_RemoveItemFromPlayer));
    Hooks::RedirectCall(L"AFortAthenaMutator_Ashton villain death -> UFortKismetLibrary::K2_RemoveItemFromPlayer", Sites.VillainRemoveCall, reinterpret_cast<void*>(&UFortKismetLibrary::K2_RemoveItemFromPlayer));
    Log::Info(L"Endgame: the gauntlet's give and the leader's and villains' item removals are stripped, restored at 3 calls; stone capture {}, team loadouts {}",
        bPickupGiven ? L"on" : L"off", bStartingItems ? L"on" : L"off");
#endif
}
