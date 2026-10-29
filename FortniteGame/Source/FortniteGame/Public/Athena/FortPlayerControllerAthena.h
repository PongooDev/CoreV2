#pragma once

#include "Containers/Array.h"
#include "Items/FortItemDefinition.h"
#include "Math/Vector.h"
#include "Player/FortPlayerController.h"

#include <vector>

class AActor;
class AFortPawn;
class AFortPlayerPawnAthena;
class AFortPlayerStateAthena;
class UFortHero;
class UFortHeroType;
struct FGameplayTagContainer;

// A Battle Royale outfit (from 1.8; CID_*): the hero it plays (HID_*_Athena_Commando_*).
class UAthenaCharacterItemDefinition : public UFortItemDefinition
{
    DefineUnrealClass(UAthenaCharacterItemDefinition, FortniteGame)

public:
    DefineUProperty(UFortHeroType*, HeroDefinition)
};

// A Battle Royale pickaxe (from 1.8; DefaultPickaxe, Pickaxe_ID_*): the harvesting tool it gives.
class UAthenaPickaxeItemDefinition : public UFortItemDefinition
{
    DefineUnrealClass(UAthenaPickaxeItemDefinition, FortniteGame)

public:
    DefineUProperty(UFortWeaponItemDefinition*, WeaponDefinition)
};

// A Battle Royale glider (from 1.8; DefaultGlider, Glider_ID_*).
class UAthenaGliderItemDefinition : public UFortItemDefinition
{
    DefineUnrealClass(UAthenaGliderItemDefinition, FortniteGame)
};

// The cosmetics a player wears in Battle Royale (from 1.8), from their athena profile's favorite_* items.
struct FFortAthenaLoadout
{
    DefineUnrealStruct(FFortAthenaLoadout, FortniteGame)
    DefineUProperty(UAthenaGliderItemDefinition*, Glider)
    DefineUProperty(UAthenaPickaxeItemDefinition*, Pickaxe)
    DefineUProperty(UAthenaCharacterItemDefinition*, Character)

    static bool HasGlider() { return Member_Glider.IsAvailable(); }
};

// Battle Royale's player controller (from 1.7.2; Athena_PlayerController_C).
class AFortPlayerControllerAthena : public AFortPlayerController
{
    DefineUnrealClass(AFortPlayerControllerAthena, FortniteGame)

public:
    // The heroes 1.7.2's Battle Royale plays (Athena_PlayerController_C: HID_Commando_Athena_*). 1.8 leaves it empty.
    DefineUProperty(TArray<UFortHeroType*>, DefaultHeroes)
    // 1.8: the player's loadout. Athena_PlayerController_C's default has DefaultGlider and DefaultPickaxe in it.
    DefineUStructProperty(FFortAthenaLoadout, CustomizationLoadout)
    // Client: the player's team. The HUD's team list takes their talking states in this order (GetTeamTalking, 1.7.2
    // 0x430440, and the controller's tick, 0x417630, which tells the HUD when they change).
    DefineUProperty(TArray<AFortPlayerStateAthena*>, TeamMembers)

    // Whether the controller plays Battle Royale heroes only: 1.7.2's DefaultHeroes, or 1.8's loadout's.
    bool PlaysBattleRoyaleHeroes();
    bool IsDefaultHero(UFortHero* Hero);
    // 1.8: the hero of the loadout's character, once GetOrInitializeHero has made the loadout.
    UFortHeroType* GetLoadoutHero();
    // 1.8: the harvesting tool of the loadout's pickaxe, or of the class's default loadout's (DefaultPickaxe). 1.7.2
    // has none: its pickaxe is one of the game mode's StartingItems.
    UFortWorldItemDefinition* GetLoadoutPickaxe();
    // 1.8: puts the class's default loadout's glider and pickaxe where the loadout, and Pawn's copy of it, have none.
    void FillEmptyLoadout(AFortPlayerPawnAthena* Pawn);
    UFortHero* GetOrInitializeHero();
    UFortHero* SetupHero();
    void SetupBackpackSize();
    void GiveLoadoutPickaxe();
    // Takes back what the player picked up in warmup: the inventory is the game mode's StartingItems again.
    void ResetToStartingItems();

    // An eliminated player's inventory lies around where they died: everything that can be dropped, one pickup each.
    void DropInventoryOnDeath(AFortPawn* DeadPawn);
    // The spots on the floor around DeathLocation that DropInventoryOnDeath tosses Count pickups to (DeadPawn is the
    // traces' world context and is ignored by them).
    static std::vector<FVector> GetInventoryDropSpots(AActor* DeadPawn, const FVector& DeathLocation, int32_t Count);
    // Server: the controller of a player who just died stands over their body, and a couple of frames later, once the
    // game's own death handling is done, their client's does too (the minimap follows it, #139). The camera stays the
    // game's death camera (#125).
    void PlaceAtDeathLocation(AFortPawn* DeadPawn);

    static void HookCallSites();

private:
    static void PawnDiedInventoryDropCall(AFortPlayerController* Controller, void* Unused, const FGameplayTagContainer* DamageTags, AFortPawn* DeadPawn, bool* bOutDroppedBackpack);
};
