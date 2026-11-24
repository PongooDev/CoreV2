#pragma once

#include "Containers/Array.h"
#include "GameFramework/Info.h"
#include "Math/Vector.h"
#include "Reflection.h"
#include "UObject/Class.h"

class AFortAthenaVehicle;
class UClass;

// Air Royale's mutator (8.40; Playlist_Goose_*'s Mutator_Goose_C). The playlist has no Battle Bus: at the aircraft
// phase each squad is put in an X-4 Stormwing (VehicleClass, LTMFerretVehicle_C) high over the island, and a squad
// respawns in a plane while it has lives (NumberOfPlanesPerSquad).
//
// What 8.40 (CL-6005771) keeps of it, all native: BeginPlay (0xFD1F00) binds OnGamePhaseChanged to the game state's
// GamePhaseChanged and OnPlaylistLoaded to its OnCurrentPlaylistLoaded; the game mode's tick (0xF3CC20 -> slot 0xCD,
// 0xFDA770) adds every player controller to its squad's SquadInfoArray entry; the aircraft phase (OnGamePhaseChanged,
// 0xFE7900) starts the pickup timer (HowOftenToUpdatePickupCount, 0xFF5D80) and restarts every squad's players;
// ChoosePlayerStart gives a player their squad's plane (slot 0xE2, 0xFDA110); the game state's
// HandlePlacePlayersInAircraft is answered (slot 0x11C, nobody boards a bus); and the game state's OnClientPawnLoaded
// (slot 0x113, 0xFDD6A0) seats a new pawn in its squad's plane (SeatPlayer, 0xFEDF90: the squad's plane, else a
// sleeping one, else a new one; teleported into the sky and entered). IsRespawningAllowed (slot 0xE5) is the squad's
// PlanesRemaining, OnVehicleDied gives a squad with lives left another plane, OnVehicleStolen gives the robbed squad one.
//
// What it doesn't have is SpawnPlane (Core's name; 0xFF2460): the build keeps only its loop over SpawnedVehicles for
// the asleep plane furthest along X, and returns null. So no plane was ever spawned: OnPlaylistLoaded parked none,
// SeatPlayer found none ("How did we get here?!") and left the pawn on the ground, and a respawn had no plane either.
class AFortAthenaMutator_Goose : public AInfo
{
    DefineUnrealClass(AFortAthenaMutator_Goose, FortniteGame)
    DefineUFunction(OnVehicleDied)
    DefineUFunction(OnVehicleStolen)

public:
    DefineUProperty(FVector, PlaneSpawnLocation)
    DefineUProperty(UClass*, VehicleClass)
    DefineUProperty(TArray<AFortAthenaVehicle*>, SpawnedVehicles)
    static bool HasPlaneSpawning()
    {
        // 8.40's shape: OnVehicleStolen(uint8 OldID, uint8 NewID), as the vehicle's OnVehicleSquadChanged sends it. 7.30 to
        // 8.30 have the mutator while Air Royale was being made (8.30's takes int32 squad ids); no playlist of theirs uses it.
        UFunction* Stolen = StaticClass() ? FindStaticFunction_OnVehicleStolen() : nullptr;
        return Stolen && Stolen->ParmsSize == 2 && Member_PlaneSpawnLocation.IsAvailable() && Member_VehicleClass.IsAvailable() && Member_SpawnedVehicles.IsAvailable()
            && FindStaticFunction_OnVehicleDied();
    }

    // A new plane for the mutator, asleep in the row at PlaneSpawnLocation (130000 up), its death and squad change bound
    // to OnVehicleDied and OnVehicleStolen, in SpawnedVehicles. SeatPlayer teleports it into the sky when it seats a
    // squad in it.
    AFortAthenaVehicle* SpawnPlane();

    // From 8.40: SpawnPlane at the build's four calls to the stripped body (OnPlaylistLoaded, SeatPlayer,
    // OnVehicleDied, OnVehicleStolen), and the pickup timer's stripped spawn (AAthenaGoosePickup::SpawnGoosePickup).
    // Nothing on builds without Air Royale.
    static void HookNatives();

private:
    // Core's: how far apart the sleeping planes are parked along X. The build keeps only the search for the furthest
    // one (from PlaneSpawnLocation), not the step past it.
    static constexpr float SleepingPlaneSpacing = 1500.0f;

    static AFortAthenaVehicle* SpawnPlaneCall(AFortAthenaMutator_Goose* This);
};
