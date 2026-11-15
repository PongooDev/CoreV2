#pragma once

#include "Building/BuildingActor.h"
#include "Delegates/Delegate.h"

class UClass;
class UWorld;
struct FVector;

class EBarrierState
{
    DefineUnrealEnum(EBarrierState, FortniteGame)
    DefineUEnumValue(BarrierUp)
    DefineUEnumValue(BarrierComingDown)
    DefineUEnumValue(BarrierDown)
};

// Food Fight's barrier (from 6.30; Barrier_C): the wall down the middle of the map, an ABuildingGameplayActor (Core
// declares none, so this derives from its ABuildingActor). The Barrier mutator's OnGamePhaseStepChanged (6.30
// 0xD46DC0) starts its fall: in the storm circle before Barrier.Wall.ZonePhaseToBringDown it calls SetWallDropTimer
// (0xC58120) with TimeToBringDownWall and WallGravity, which shows TimeUntilWallComesDown on the wall and, when it is
// over, broadcasts the wall's starting-to-come-down delegate and gives the wall its gravity (0xC58380, BarrierState
// BarrierComingDown). Its UAthenaBigBaseWallMovementComp (0xC400E0) lowers it until it is under ZKillLevel, sets
// BarrierDown and broadcasts the wall-down delegate. Those natives are all in the build; spawning it and the mutator's
// side of the two delegates aren't.
class AAthenaBigBaseWall : public ABuildingActor
{
    DefineUnrealClass(AAthenaBigBaseWall, FortniteGame)

public:
    DefineUProperty(EBarrierState, BarrierState)

    // The wall's two native delegates (DECLARE_MULTICAST_DELEGATE, no parameters), the last of its unreflected members:
    // the one SetWallDropTimer's timer broadcasts as the wall starts to fall (6.30 0xC42539: "lea rcx, [rax+790h]"),
    // and the one its movement broadcasts once it is down (0xC402D9: +0x7A8). Null where the class isn't in the build
    // or its layout isn't the one Core knows (the two lists aren't empty and unlocked when the mutator binds them).
    TMulticastDelegate<>* GetOnWallStartingToComeDown() const;
    TMulticastDelegate<>* GetOnWallDown() const;

    // 21.40's AAthenaBigBaseWall::CreateWall: a wall of BigBaseWallClass across the map, from MidlineStart to MidlineEnd
    // at ZLevel, facing along the line (its forward axis is the wall's length: Barrier_C's audio follows
    // the line through its location along GetForwardVector).
    static AAthenaBigBaseWall* CreateWall(UWorld* World, UClass* BigBaseWallClass, const FVector& MidlineStart, const FVector& MidlineEnd, float ZLevel);

private:
    TMulticastDelegate<>* GetNativeDelegate(int32_t OffsetFromEnd) const;
};
