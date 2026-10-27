#pragma once

#include "Containers/Array.h"
#include "Player/FortPlayerController.h"

class AFortPawn;
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

    bool IsDefaultHero(UFortHero* Hero);
    UFortHero* GetOrInitializeHero();
    UFortHero* SetupHero();
    void SetupBackpackSize();
    // Takes back what the player picked up in warmup: the inventory is the game mode's StartingItems again.
    void ResetToStartingItems();

    // An eliminated player's inventory lies around where they died: everything that can be dropped, one pickup each.
    void DropInventoryOnDeath(AFortPawn* DeadPawn);

    static void HookCallSites();

private:
    static void PawnDiedInventoryDropCall(AFortPlayerController* Controller, void* Unused, const FGameplayTagContainer* DamageTags, AFortPawn* DeadPawn, bool* bOutDroppedBackpack);
};
