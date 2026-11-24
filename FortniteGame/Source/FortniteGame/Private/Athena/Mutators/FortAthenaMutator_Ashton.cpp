#include "pch.h"
#include "Athena/Mutators/FortAthenaMutator_Ashton.h"
#include "Athena/FortGameStateAthena.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "Athena/FortPlayerStateAthena.h"
#include "Athena/FortPlaylistAthena.h"
#include "Engine/World.h"
#include "Finder.h"
#include "FortGameModeZone.h"
#include "FortKismetLibrary.h"
#include "Hooks.h"
#include "Items/FortPickup.h"
#include "Items/FortInventoryOwnerInterface.h"
#include "Items/FortItemDefinition.h"
#include "Log.h"
#include "Logging/LogMacros.h"
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

void AFortAthenaMutator_Ashton::GiveVillainLeaderItemCall(AFortPlayerControllerAthena* NewVillainLeader)
{
#if WITH_SERVER_CODE
    AFortAthenaMutator_Ashton* Mutator = NewVillainLeader ? GetCurrent(NewVillainLeader->GetWorld()) : nullptr;
    if (!Mutator || static_cast<AFortPlayerControllerAthena*>(Mutator->VillainLeaderPC) != NewVillainLeader)
    {
        return;
    }
    UFortWorldItemDefinition* LeaderItem = static_cast<UFortGadgetItemDefinition*>(Mutator->VillainLeaderItemDef);
    // Core's: bNotifyPlayer false (the build's FFortItemEntry(VillainLeaderItemDef, 1) says nothing of it).
    UFortKismetLibrary::K2_GiveItemToPlayer(NewVillainLeader, LeaderItem, 1, false);
    UE_LOG(LogAshtonMutator, Log, TEXT("PromoteVillainLeader: %s is Thanos, given %s"), *NewVillainLeader->GetName(), *GetNameSafe(LeaderItem));
#endif
}

void AFortAthenaMutator_Ashton::OnPickupSpawnedAndReady(AFortPickup* PickupObj, const UFortWorldItemDefinition* PickupItemDef)
{
#if WITH_SERVER_CODE
    if (Role != ROLE_Authority || !PickupItemDef || !UFortWorldItemDefinition::HasSpecialActorPickupTag())
    {
        return;
    }
    // The stone whose GameplayTag the pickup's tag is under (SpecialActor.Ashton.Object.P.Pickup under .Object.P), as
    // OnPickupDestroying's lookup (0x1067230) matches them.
    const std::wstring PickupTag = static_cast<FGameplayTag>(const_cast<UFortWorldItemDefinition*>(PickupItemDef)->SpecialActorPickupTag).TagName.ToString().ToStdString();
    const int32_t DataSize = FAshtonStoneData::StaticStruct() ? FAshtonStoneData::StaticStruct()->GetStructureSize() : 0;
    const uint8_t* DataList = reinterpret_cast<const uint8_t*>(&static_cast<FAshtonStoneData&>(StoneDataList));
    int32_t Found = -1;
    for (int32_t Index = 0; DataSize > 0 && Index < 6 && Found < 0; ++Index)
    {
        const FAshtonStoneData& Data = *reinterpret_cast<const FAshtonStoneData*>(DataList + static_cast<ptrdiff_t>(Index) * DataSize);
        const std::wstring StoneTag = static_cast<FGameplayTag>(const_cast<FAshtonStoneData&>(Data).GameplayTag).TagName.ToString().ToStdString();
        if (!StoneTag.empty() && (PickupTag == StoneTag || PickupTag.starts_with(StoneTag + L".")))
        {
            Found = Index;
        }
    }
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

    Finder::FAshtonMutatorSites Sites;
    if (!Finder::ResolveAshtonMutatorLive(Member_VillainLeaderPC.GetOffsetChecked(), Member_VillainLeaderItemDef.GetOffsetChecked(), Member_VillainItemDefs.GetOffsetChecked(), Sites))
    {
        Log::Info(L"Endgame: the gauntlet's give and the death removals are not restored; stone capture {}, team loadouts {}", bPickupGiven ? L"on" : L"off", bStartingItems ? L"on" : L"off");
        return;
    }
    if (Sites.PickupSpawnedAndReadyLea && FAshtonStoneState::StaticStruct() && FAshtonStoneData::StaticStruct() && Member_StoneList.IsAvailable() && Member_StoneDataList.IsAvailable())
    {
        Hooks::RedirectAddress(L"AFortAthenaMutator_Ashton::BeginPlay's OnPickupSpawnedAndReady binding", Sites.PickupSpawnedAndReadyLea, reinterpret_cast<void*>(&OnPickupSpawnedAndReadyHK));
        if (UFunction* Function = FindObject<UFunction>(nullptr, L"/Script/FortniteGame.FortAthenaMutator_Ashton:OnPickupSpawnedAndReady"))
        {
            Function->SetNativeFunc(&execOnPickupSpawnedAndReady);
        }
    }
    Hooks::RedirectCall(L"AFortAthenaMutator_Ashton promotion -> UFortKismetLibrary::K2_GiveItemToPlayer (VillainLeaderItemDef)", Sites.LeaderGiveCall, reinterpret_cast<void*>(&GiveVillainLeaderItemCall));
    Hooks::RedirectCall(L"AFortAthenaMutator_Ashton villain leader death -> UFortKismetLibrary::K2_RemoveItemFromPlayer", Sites.LeaderRemoveCall, reinterpret_cast<void*>(&UFortKismetLibrary::K2_RemoveItemFromPlayer));
    Hooks::RedirectCall(L"AFortAthenaMutator_Ashton villain death -> UFortKismetLibrary::K2_RemoveItemFromPlayer", Sites.VillainRemoveCall, reinterpret_cast<void*>(&UFortKismetLibrary::K2_RemoveItemFromPlayer));
    Log::Info(L"Endgame: the gauntlet's give and the leader's and villains' item removals are stripped, restored at 3 calls; stone capture {}, team loadouts {}",
        bPickupGiven ? L"on" : L"off", bStartingItems ? L"on" : L"off");
#endif
}
