#pragma once

#include "AI/FortAIGoalManager.h"
#include "UObject/Stack.h"

class AActor;
class UFunction;
class UWorld;

// From 6.x, the controller of Battle Royale's own AI (6.20/6.21's Fortnitemares Cube Monsters). Its Possess (6.21
// CL-4526925 slot 0xD1, 0xA9EF40) puts it in the world's UAthenaAISystem::AIControllers once AFortAIController's
// Possess (0xA9F360) has run, whether or not that took the pawn (it takes none that is pending kill), and keeps its
// index in the controller (+0x558 on 6.20-6.22): UAthenaAISystem::RegisterManagedController, 21.40's name. Its
// UnPossess (slot 0xD2, 0xAA6A90) takes it out again (UAthenaAISystem::UnregisterManagedController, 21.40's name;
// 0xAA6D80, which also takes its behavior tree and path following components out of the system's other two lists),
// but only while it still has a pawn. 6.21 and 6.22 add a last try in its BeginDestroy (0xA899F0); 6.20 has none.
//
// A controller destroyed without a pawn while still in the list (AController::Destroyed's UnPossess has nothing to let
// go) stays in it pending kill, and the garbage collector clears its entry: AIControllers is a UPROPERTY. The next
// unregister then moves that null entry into the slot it frees and writes the moved controller's index through it.
// #447: 6.21, a 93-bot Deimos squads match, crashed in the garbage collector's BeginDestroy of such a controller
// (0xAA6E71, writing null+0x558; the call from 0xA899F0 under UnhashUnreachableObjects 0x190C3F0). 6.20 would crash
// in the next UnPossess instead.
class AAthenaAIController : public AFortAIController
{
    DefineUnrealClass(AAthenaAIController, FortniteGame)

public:
    // Has each of the world's Athena AI controllers, the ones already there and each one spawned later
    // (UWorld::OnActorSpawned), take itself out of the AI system as it is destroyed (AActor::OnDestroyed, which
    // AActor::Destroyed broadcasts after AController::Destroyed's UnPossess, before the actor is pending kill). Once per
    // world; nothing on builds without the class, its OnDestroyed or the build's unregister.
    static void BindDestroyedForWorld(UWorld* World);

private:
    void BindDestroyed();
    // AActor::OnDestroyed: UAthenaAISystem::UnregisterManagedController, as the controller's UnPossess would have done
    // had it still had a pawn. Nothing for one the UnPossess took out (the build's own test of the index).
    void HandleDestroyed(AActor* DestroyedActor);
    DECLARE_FUNCTION(execHandleDestroyed);

    static UFunction* HandleDestroyedFunction;
};
