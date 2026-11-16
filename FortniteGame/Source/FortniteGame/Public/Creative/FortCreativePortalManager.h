#pragma once

#include "Containers/Array.h"
#include "GameFramework/Info.h"
#include "Reflection.h"

class AFortAthenaCreativePortal;
class AFortPlayerControllerAthena;
class AFortPlayerState;

// The hub's rifts (from 7.00): every rift registers with it as it begins play (RegisterPortal, 0xC1ECD0), the hub's
// own rifts as AvailablePortals; MarkPortalUsed and ReleasePortal move one between AvailablePortals and UsedPortals.
class AFortCreativePortalManager : public AInfo
{
    DefineUnrealClass(AFortCreativePortalManager, FortniteGame)
    DefineUFunction(MarkPortalUsed)
    DefineUFunction(ReleasePortal)

public:
    DefineUProperty(TArray<AFortAthenaCreativePortal*>, AllPortals)
    DefineUProperty(TArray<AFortAthenaCreativePortal*>, UsedPortals)
    DefineUProperty(TArray<AFortAthenaCreativePortal*>, AvailablePortals)

    void MarkPortalUsed(AFortAthenaCreativePortal* PortalUsed);
    void ReleasePortal(AFortAthenaCreativePortal* PortalToRelease);

    // Restored (7.00 strips it with its only caller; "Out of available portals!" is left, unreferenced): a joining
    // player takes the first free rift of the hub, which shows their name and opens on their island once it loads.
    AFortAthenaCreativePortal* ClaimPortal(AFortPlayerControllerAthena* PlayerController);
    // A leaving player's rift: their island is saved, checked in and cleared, and the rift is free for the next one.
    void ReleasePlayerPortal(AFortPlayerState* PlayerState);

    static AFortCreativePortalManager* Get(const UObject* WorldContextObject);
    // Core's: what the match's hub has, once the Creative playlist has loaded (its rifts, their islands and saves).
    static void LogHub(const UObject* WorldContextObject);
};
