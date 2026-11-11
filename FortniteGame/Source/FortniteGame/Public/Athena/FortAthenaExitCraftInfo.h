#pragma once

#include "GameplayEffect.h"
#include "GameplayTagContainer.h"
#include "Math/Vector2D.h"
#include "Styling/SlateBrush.h"
#include "UObject/Object.h"

// How an exit craft comes in and leaves (from 5.40; FortAthenaExitCraftTypes.h). Its values are the craft info's
// (Bling_VanInfo: OverrideGameData's VanInfo.* rows).
struct FExitCraftInfo
{
    DefineUnrealStruct(FExitCraftInfo, FortniteGame)
    // How long the spawner stands before its craft comes (VanInfo.SpawnDelay): the spawner's BeginPlay (5.40 0xBAACB0)
    // and StartExitCraftSpawnTimer (0xBCAF80) set its ExitCraftTimerHandle for it.
    DefineUStructProperty(FScalableFloat, ExitCraftSpawnDelay)
    // How high above its spawner the craft appears (VanInfo.SpawnHeight): the van comes out of a portal in the sky and
    // falls to the spawner's spot (its Spawned state, 0xBD5830, waits for it to slow to MinLandingSpeed).
    DefineUStructProperty(FScalableFloat, ExitCraftZOffset)
};

// The Getaway's van (from 5.40; Bling_VanInfo, which Mutator_BlingMode_C's ExitCraftInfo names): the craft and its
// spawner, and the icons the map and the compass show for each (special actors).
class UFortAthenaExitCraftInfo : public UObject
{
    DefineUnrealClass(UFortAthenaExitCraftInfo, FortniteGame)

public:
    // Bling_Van_C (Epic's spelling).
    DefineUProperty(UClass*, ExitCaftClass)
    DefineUStructProperty(FExitCraftInfo, ExitCraftInfo)
    // The spawner's icons (SpecialActor.Heist.ExitCraft.Spawner, M_UI_Heist_OTW2): what AFortAthenaExitCraftSpawner::
    // RegisterSpawnerSpecialActor (5.40 0xBC4A10) registers it with.
    DefineUProperty(FGameplayTag, SpecialActorSpawnerTag)
    // The van's (SpecialActor.Heist.ExitCraft.Vehicle; the map's M_UI_Heist_Ping_Van, the compass's M_UI_Heist_Van).
    DefineUProperty(FGameplayTag, SpecialActorCraftTag)
    DefineUStructProperty(FSlateBrush, CraftMinimapIconBrush)
    DefineUProperty(FVector2D, CraftMinimapIconScale)
    DefineUStructProperty(FSlateBrush, CraftCompassIconBrush)
    DefineUProperty(FVector2D, CraftCompassIconScale)
};
