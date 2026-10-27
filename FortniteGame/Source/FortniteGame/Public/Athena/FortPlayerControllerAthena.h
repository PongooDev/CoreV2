#pragma once

#include "Containers/Array.h"
#include "Math/Vector.h"
#include "Player/FortPlayerController.h"

#include <vector>

class AActor;
class AFortPawn;
class AFortPlayerStateAthena;
class UFortHero;
class UFortHeroType;
struct FGameplayTagContainer;

// Battle Royale's player controller (from 1.7.2; Athena_PlayerController_C).
class AFortPlayerControllerAthena : public AFortPlayerController
{
    DefineUnrealClass(AFortPlayerControllerAthena, FortniteGame)

public:
    // The heroes Battle Royale plays (Athena_PlayerController_C: HID_Commando_Athena_*).
    DefineUProperty(TArray<UFortHeroType*>, DefaultHeroes)
    // Client: the player's team. The HUD's team list takes their talking states in this order (GetTeamTalking, 1.7.2
    // 0x430440, and the controller's tick, 0x417630, which tells the HUD when they change).
    DefineUProperty(TArray<AFortPlayerStateAthena*>, TeamMembers)

    bool IsDefaultHero(UFortHero* Hero);
    UFortHero* GetOrInitializeHero();
    UFortHero* SetupHero();
    void SetupBackpackSize();
    // Takes back what the player picked up in warmup: the inventory is the game mode's StartingItems again.
    void ResetToStartingItems();

    // An eliminated player's inventory lies around where they died: everything that can be dropped, one pickup each.
    void DropInventoryOnDeath(AFortPawn* DeadPawn);
    // The spots on the floor around DeathLocation that DropInventoryOnDeath tosses Count pickups to (DeadPawn is the
    // traces' world context and is ignored by them).
    static std::vector<FVector> GetInventoryDropSpots(AActor* DeadPawn, const FVector& DeathLocation, int32_t Count);

    static void HookCallSites();

private:
    static void PawnDiedInventoryDropCall(AFortPlayerController* Controller, void* Unused, const FGameplayTagContainer* DamageTags, AFortPawn* DeadPawn, bool* bOutDroppedBackpack);
};
