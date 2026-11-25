#include "pch.h"
#include "Athena/Mutators/FortAthenaMutator.h"
#include "Athena/AI/CoreAthenaBotManager.h"
#include "Athena/FortGameModeAthena.h"
#include "Athena/FortGameStateAthena.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "Athena/FortPlayerStateAthena.h"
#include "Finder.h"
#include "FortKismetLibrary.h"
#include "FortTeamInfo.h"
#include "GameFramework/PlayerController.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "Items/FortItemDefinition.h"
#include "Pawns/FortPawn.h"
#include "Player/FortPlayerController.h"

#include <cmath>
#include <vector>

bool AFortAthenaMutator::HasOnPostEarnedKill()
{
#if WITH_SERVER_CODE
    UClass* const PlayerControllerClass = APlayerController::StaticClass();
    return StaticClass() && AFortGameStateAthena::HasMutatorGenericInts() && PlayerControllerClass
        && Finder::ResolveOnPostEarnedKillLive(AFortGameStateAthena::MutatorGenericInt0Offset(), AFortGameStateAthena::MutatorGenericInt1Offset(),
            PlayerControllerClass->GetPropertiesSize());
#else
    return false;
#endif
}

void AFortAthenaMutator::FortGameStateAthena_OnPostEarnedKill(AFortPlayerController* KillerController)
{
#if WITH_SERVER_CODE
    if (HasOnPostEarnedKill() && KillerController)
    {
        CallVirtual<void>(this, Natives::AFortAthenaMutator_OnPostEarnedKillIndex, KillerController);
    }
#endif
}

uint8_t AFortAthenaMutator::FortGameModeAthena_CheckWinConditionOnPlayerRemoval(AFortPlayerControllerAthena* Player, APlayerState* KillerPlayerState, int32_t PlayersLeft,
    APawn* FinisherPawn, const UFortWeaponItemDefinition* FinishingWeapon, uint8_t DeathCause)
{
#if WITH_SERVER_CODE
    if (Finder::ResolveCheckWinConditionOnPlayerRemovalLive())
    {
        return CallVirtual<uint8_t>(this, Natives::AFortAthenaMutator_CheckWinConditionOnPlayerRemovalIndex, Player, KillerPlayerState, PlayersLeft, FinisherPawn, FinishingWeapon, DeathCause);
    }
#endif
    return 0;
}

int32_t AFortAthenaMutator::GiveItemToAllPlayers(UFortWorldItemDefinition* ItemDefinition, int32_t Count) const
{
#if WITH_SERVER_CODE
    AFortGameModeAthena* GameMode = AFortGameModeAthena::GetCurrent(this);
    if (!GameMode || !ItemDefinition || Count <= 0)
    {
        return 0;
    }
    int32_t Given = 0;
    const TArray<AFortPlayerControllerAthena*>& Alive = GameMode->AlivePlayers;
    for (int32_t Index = 0; Index < Alive.Num(); ++Index)
    {
        AFortPlayerControllerAthena* PlayerController = Alive[Index];
        if (!PlayerController || PlayerController->IsPendingKill())
        {
            continue;
        }
        // Core's: a weapon comes loaded, as the game's grants of a mutator's items do.
        UFortKismetLibrary::GiveItemToInventoryOwner(TScriptInterface<IFortInventoryOwnerInterface>(PlayerController, nullptr), ItemDefinition, Count, false, 1, true);
        ++Given;
    }
    // Core's player bots aren't in AlivePlayers (their controllers are stock AAIControllers).
    if (FCoreAthenaBotManager* Bots = FCoreAthenaBotManager::Get())
    {
        Given += Bots->GiveItemToBots(ItemDefinition, Count);
    }
    return Given;
#else
    return 0;
#endif
}

void AFortAthenaMutator_TDM::ScoreBotKill(AFortGameStateAthena* GameState, AFortPlayerStateAthena* KillerPlayerState)
{
#if WITH_SERVER_CODE
    // As 0xD3C1B0: the earner's team's team info, if it is an AFortTeamInfoAthena; its score one more; with
    // bIsTwoTeamTDM, the score into the game state's int for that team.
    AFortTeamInfo* TeamInfo = KillerPlayerState && GameState ? static_cast<AFortTeamInfo*>(KillerPlayerState->PlayerTeam) : nullptr;
    UClass* const TeamInfoAthenaClass = AFortTeamInfoAthena::StaticClass();
    if (!TeamInfo || !TeamInfoAthenaClass || !reinterpret_cast<UObject*>(TeamInfo)->IsA(TeamInfoAthenaClass) || Natives::AFortTeamInfoAthena_CurrentTeamScore < 0)
    {
        return;
    }
    int32_t& CurrentTeamScore = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(TeamInfo) + Natives::AFortTeamInfoAthena_CurrentTeamScore);
    ++CurrentTeamScore;
    if (!bIsTwoTeamTDM)
    {
        return;
    }
    if (static_cast<uint8_t>(KillerPlayerState->TeamIndex) == EFortTeam::GetHumanPvP_Team1())
    {
        GameState->MutatorGenericInt_0 = CurrentTeamScore;
    }
    else
    {
        GameState->MutatorGenericInt_1 = CurrentTeamScore;
    }
#endif
}

bool AFortAthenaMutator_InventoryOverride::ShouldDropBotItem(const UFortItemDefinition* ItemDefinition, bool& bOutShouldDrop) const
{
#if WITH_SERVER_CODE
    // 0xD3C4C0 (6.31 CL-4573279) goes by the item's type (its definition's GetItemType, slot 0x4E). A DropAllItemsOverride
    // other than NoOverride answers for ammo, consumables, ingredients, gadgets, traps, melee and ranged weapons and
    // resources (ForceDrop drops, ForceKeep keeps); with NoOverride, the override for the item's own kind answers
    // (ranged weapons, resources, traps, gadgets, consumables, ammo), and NoOverride there has no say. The
    // definition's ItemType is what its GetItemType returns for the kinds a bot carries.
    if (!ItemDefinition || !HasDropOverrides() || !UFortItemDefinition::StaticClass())
    {
        return false;
    }
    const uint8_t ItemType = ItemDefinition->ItemType;
    const auto Is = [ItemType](uint8_t Type) { return Type != Reflection::InvalidEnumValue && ItemType == Type; };
    const uint8_t NoOverride = EAthenaLootDropOverride::GetNoOverride();
    const uint8_t ForceDrop = EAthenaLootDropOverride::GetForceDrop();
    if (NoOverride == Reflection::InvalidEnumValue || ForceDrop == Reflection::InvalidEnumValue)
    {
        return false;
    }
    const uint8_t DropAll = DropAllItemsOverride;
    if (DropAll != NoOverride)
    {
        if (Is(EFortItemType::GetAmmo()) || Is(EFortItemType::GetConsumable()) || Is(EFortItemType::GetIngredient()) || Is(EFortItemType::GetAthenaGadget()) || Is(EFortItemType::GetTrap())
            || Is(EFortItemType::GetWeaponMelee()) || Is(EFortItemType::GetWeaponRanged()) || Is(EFortItemType::GetWorldResource()))
        {
            bOutShouldDrop = DropAll == ForceDrop;
            return true;
        }
        return false;
    }
    uint8_t Override = NoOverride;
    if (Is(EFortItemType::GetWeaponRanged()))
    {
        Override = WeaponDropOverride;
    }
    else if (Is(EFortItemType::GetWorldResource()))
    {
        Override = MaterialDropOverride;
    }
    else if (Is(EFortItemType::GetTrap()))
    {
        Override = TrapDropOverride;
    }
    else if (Is(EFortItemType::GetAthenaGadget()))
    {
        Override = GadgetDropOverride;
    }
    else if (Is(EFortItemType::GetConsumable()))
    {
        Override = ConsumableDropOverride;
    }
    else if (Is(EFortItemType::GetAmmo()))
    {
        Override = AmmoDropOverride;
    }
    if (Override == NoOverride)
    {
        return false;
    }
    bOutShouldDrop = Override == ForceDrop;
    return true;
#else
    return false;
#endif
}

bool AFortAthenaMutator_InventoryOverride::HasActiveLoadouts(const AFortGameStateAthena* GameState)
{
#if WITH_SERVER_CODE
    UClass* const OverrideClass = StaticClass();
    const TArray<UObject*>* MutatorList = GameState && OverrideClass && HasInventoryLoadouts() ? GameState->GetGameplayMutatorList() : nullptr;
    for (int32_t Index = 0; MutatorList && Index < MutatorList->Num(); ++Index)
    {
        UObject* Mutator = (*MutatorList)[Index];
        if (!Mutator || !Mutator->IsA(OverrideClass) || (AFortAthenaMutator::HasMutatorActive() && !reinterpret_cast<AFortAthenaMutator*>(Mutator)->bMutatorActive))
        {
            continue;
        }
        const AFortAthenaMutator_InventoryOverride* Override = reinterpret_cast<const AFortAthenaMutator_InventoryOverride*>(Mutator);
        const TStructArray<FItemLoadoutContainer>& Loadouts = Override->InventoryLoadouts;
        if (Loadouts.Num() > 0)
        {
            return true;
        }
    }
#endif
    return false;
}

bool AFortAthenaMutator_InventoryOverride::GetInventoryOverrideForBot(bool bInitialSpawn, uint8_t TeamIndex, const AFortGameStateAthena* GameState, TArray<const FItemAndCount*>& OutItems) const
{
#if WITH_SERVER_CODE
    if (!HasInventoryLoadouts())
    {
        return false;
    }
    const uint8_t Always = EAthenaInventorySpawnOverride::GetAlways();
    const uint8_t InitialSpawn = EAthenaInventorySpawnOverride::GetIntialSpawn();
    const uint8_t AircraftPhaseOnly = EAthenaInventorySpawnOverride::GetAircraftPhaseOnly();
    const uint8_t ForceDrop = EAthenaLootDropOverride::GetForceDrop();
    if (Always == Reflection::InvalidEnumValue || ForceDrop == Reflection::InvalidEnumValue)
    {
        return false;
    }
    const TStructArray<FItemLoadoutContainer>& Loadouts = InventoryLoadouts;
    uint8_t UpdateOverride = InventoryUpdateOverride;
    uint8_t DropAll = DropAllItemsOverride;
    // The player's team's entry (TeamIndex matches and its LoadoutIndex is a loadout), which brings its own update and
    // drop overrides; else InventoryLoadoutIndex, if it is a loadout.
    int32_t LoadoutIndex = -1;
    if (HasTeamLoadouts())
    {
        const TStructArray<FItemLoadoutTeamMap>& Teams = TeamLoadouts;
        for (int32_t Index = 0; Index < Teams.Num(); ++Index)
        {
            const FItemLoadoutTeamMap& Team = Teams[Index];
            if (Team.TeamIndex == TeamIndex && static_cast<int32_t>(Team.LoadoutIndex) < Loadouts.Num())
            {
                LoadoutIndex = Team.LoadoutIndex;
                UpdateOverride = Team.UpdateOverrideType;
                DropAll = Team.DropAllItemsOverride;
                break;
            }
        }
    }
    if (LoadoutIndex < 0)
    {
        LoadoutIndex = InventoryLoadoutIndex;
        if (LoadoutIndex < 0 || LoadoutIndex >= Loadouts.Num())
        {
            return false;
        }
    }
    const bool bAircraftPhase = GameState && AFortGameStateAthena::StaticClass() && GameState->GamePhase == EAthenaGamePhase::GetAircraft();
    if (DropAll == ForceDrop || (UpdateOverride != Always && !(bInitialSpawn && UpdateOverride == InitialSpawn) && !(UpdateOverride == AircraftPhaseOnly && bAircraftPhase)))
    {
        return false;
    }
    const TStructArray<FItemAndCount>& Items = Loadouts[LoadoutIndex].Loadout;
    for (int32_t Index = 0; Index < Items.Num(); ++Index)
    {
        OutItems.Add(&Items[Index]);
    }
    return true;
#else
    return false;
#endif
}

void AFortAthenaMutator_ItemDropOnDeath::DropBotItemsOnDeath(AFortPawn* DeadPawn, const std::map<UFortItemDefinition*, int32_t>& Ammo) const
{
#if WITH_SERVER_CODE
    if (!DeadPawn || !HasItemsToDrop() || !UFortItemDefinition::StaticClass())
    {
        return;
    }
    // Each pickup at the pawn, facing its way, as 0xD30000 spawns them (K2_SpawnPickupInWorld with the root component's
    // location and rotation, tossed, at a random rotation).
    const FVector Location = DeadPawn->GetTransform().GetLocation();
    constexpr float DegreesToRadians = 3.1415926535897932f / 180.0f;
    const float Yaw = DeadPawn->GetActorRotation().Yaw * DegreesToRadians;
    const FVector Direction(std::cos(Yaw), std::sin(Yaw), 0.0f);
    UFortWorldItemDefinition* Dropped[64] = {};
    int32_t DropCounts[64] = {};
    int32_t NumDrops = 0;
    const auto Drop = [&](UFortItemDefinition* ItemDefinition, int32_t Count)
    {
        if (ItemDefinition && Count > 0 && NumDrops < 64)
        {
            Dropped[NumDrops] = reinterpret_cast<UFortWorldItemDefinition*>(ItemDefinition);
            DropCounts[NumDrops++] = Count;
        }
    };

    // The ammo: PercentAmmoToDrop of each stack (the count times it, rounded down), which the player keeps as well.
    const float PercentAmmo = PercentAmmoToDrop.GetValueAtLevel(0.0f);
    if (PercentAmmo > 0.0f)
    {
        for (const auto& [AmmoDefinition, Count] : Ammo)
        {
            Drop(AmmoDefinition, static_cast<int32_t>(static_cast<float>(Count) * PercentAmmo));
        }
    }

    // ItemsToDrop: the ammo entries, up to MaxAmmoTypeDrops of them at random; the resource entries, up to
    // MaxResourceTypeDrops; every other entry.
    const int32_t MaxAmmoTypes = static_cast<int32_t>(MaxAmmoTypeDrops.GetValueAtLevel(0.0f));
    const int32_t MaxResourceTypes = static_cast<int32_t>(MaxResourceTypeDrops.GetValueAtLevel(0.0f));
    std::vector<const FItemsToDropOnDeath*> AmmoEntries;
    std::vector<const FItemsToDropOnDeath*> ResourceEntries;
    std::vector<const FItemsToDropOnDeath*> Picked;
    const TStructArray<FItemsToDropOnDeath>& Entries = ItemsToDrop;
    for (int32_t Index = 0; Index < Entries.Num(); ++Index)
    {
        const FItemsToDropOnDeath& Entry = Entries[Index];
        const UFortItemDefinition* ItemDefinition = reinterpret_cast<const UFortItemDefinition*>(static_cast<UFortWorldItemDefinition*>(Entry.ItemToDrop));
        const uint8_t ItemType = ItemDefinition ? static_cast<uint8_t>(ItemDefinition->ItemType) : Reflection::InvalidEnumValue;
        if (ItemType != Reflection::InvalidEnumValue && ItemType == EFortItemType::GetAmmo())
        {
            AmmoEntries.push_back(&Entry);
        }
        else if (ItemType != Reflection::InvalidEnumValue && ItemType == EFortItemType::GetWorldResource())
        {
            ResourceEntries.push_back(&Entry);
        }
        else
        {
            Picked.push_back(&Entry);
        }
    }
    const auto PickAtRandom = [&Picked](std::vector<const FItemsToDropOnDeath*>& From, int32_t Max)
    {
        for (int32_t Count = 0; Count < Max && !From.empty(); ++Count)
        {
            const size_t Index = static_cast<size_t>(FMath::Rand()) % From.size();
            Picked.push_back(From[Index]);
            From.erase(From.begin() + static_cast<ptrdiff_t>(Index));
        }
    };
    PickAtRandom(AmmoEntries, MaxAmmoTypes);
    PickAtRandom(ResourceEntries, MaxResourceTypes);
    for (const FItemsToDropOnDeath* Entry : Picked)
    {
        Drop(reinterpret_cast<UFortItemDefinition*>(static_cast<UFortWorldItemDefinition*>(Entry->ItemToDrop)), static_cast<int32_t>(Entry->NumberToDrop.GetValueAtLevel(0.0f)));
    }

    for (int32_t Index = 0; Index < NumDrops; ++Index)
    {
        UFortKismetLibrary::K2_SpawnPickupInWorld(reinterpret_cast<UObject*>(DeadPawn), Dropped[Index], DropCounts[Index], Location, Direction, 0, true, true, false);
    }
    Log::Info(L"ItemDropOnDeath: {} leaves {} stacks of ammo and items where it died", reinterpret_cast<UObject*>(DeadPawn)->GetName().ToStdString(), NumDrops);
#endif
}

bool AFortAthenaMutator_GravityMultiplier::GetGravityData(FGravityMovementData& OutData) const
{
    if (!IsAvailable())
    {
        return false;
    }
    const TArray<FGravityMovementData>& Data = CustomGravityData;
    const int32_t Index = GravityOverride;
    if (Index < 0 || Index >= Data.Num())
    {
        return false;
    }
    OutData = Data[Index];
    return true;
}
