#pragma once

#include "Components/ActorComponent.h"
#include "Containers/Array.h"
#include "Reflection.h"

// 7.20's (CL-4727874) home of the gameplay mutators, which replaces AFortGameModeAthena's and AFortGameStateAthena's
// GameplayMutators: each of them makes one as a default subobject (the game mode's ctor 0xCF7821 into +0x11E8, the game
// state's 0xD2C399 into +0x1BF8) and the playlist's modifiers register their mutators with it. The game state's code reads
// its own list as it read GameplayMutators (0xD70628: mov rax, [rcx+1BF8h]; mov r15, [rax+110h]); the game mode's walks a
// mutator iterator over the player's minigame and the game mode, which finds each one's component by class (0xD636B0).
class UFortMutatorListComponent : public UActorComponent
{
    DefineUnrealClass(UFortMutatorListComponent, FortniteGame)

public:
    DefineUProperty(TArray<UObject*>, Mutators)
    static bool HasMutators() { return Member_Mutators.IsAvailable(); }
    // Where the game's mutator loops read the list (7.20 +0x110: the iterator's 0xD6780F), -1 where there is none.
    static ptrdiff_t MutatorsOffset() { return Member_Mutators.IsAvailable() ? Member_Mutators.GetOffsetChecked() : -1; }
};
