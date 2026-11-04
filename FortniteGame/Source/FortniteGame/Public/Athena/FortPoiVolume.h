#pragma once

#include "Components/PrimitiveComponent.h"
#include "GameFramework/Volume.h"
#include "GameplayTagContainer.h"
#include "Math/Box.h"
#include "PhysicsEngine/BodySetup.h"
#include "Reflection.h"

#include <string>

class AFortPoiVolume;
class UWorld;

// A POI volume's overlap shape (3.0). Not in any physics scene: its own body, PoiBodyInstance, is made from the volume's
// brush in AFortPoiVolume::PostInitializeComponents, and its OverlapComponent override tests that body only.
class UFortPoiCollisionComponent : public UPrimitiveComponent
{
    DefineUnrealClass(UFortPoiCollisionComponent, FortniteGame)

public:
    DefineUStructProperty(FBodyInstance, PoiBodyInstance)
};

// A named location of the Battle Royale map (3.0; Season 3's challenges: land at, visit, dance at, eliminate at). The
// game mode's AFortPoiManager asks each one whether a pawn is inside it (UFortPoiCollisionComponent::OverlapComponent on
// the pawn's capsule) and gives the pawn its LocationTags (Athena.Location.POI.*, Athena.Location.Meta.*,
// Athena.Location.Quest.*).
class AFortPoiVolume : public AVolume
{
    DefineUnrealClass(AFortPoiVolume, FortniteGame)

public:
    // The volumes whose box diagonal is shorter than this (a config value) are the ones the manager's tick tests for
    // Visit (AFortPoiVolume::ShouldTickUpdate, 3.0 0xA04420).
    DefineUProperty(float, LargeGameVolume)
    DefineUProperty(FGameplayTagContainer, LocationTags)
    DefineUProperty(UFortPoiCollisionComponent*, PoiCollisionComp)

    // Gives every POI volume of World that has no brush component a box where the world's own data puts it (actor tags,
    // arena cameras, streamed level names; see FortPoiVolume.cpp), so the manager's overlap tests work. Volumes that have
    // their brush (a server cook, or server assets put back into the build) are left to Epic's PostInitializeComponents,
    // and volumes the data doesn't place are logged and left shapeless. Returns the volumes it shaped.
    static int32_t RestorePoiVolumes(UWorld* World);

    // The box RestorePoiVolumes gave it (invalid for a volume it didn't shape).
    FBox GetFallbackBox() const;

    std::wstring DescribeLocationTags() const;
};
