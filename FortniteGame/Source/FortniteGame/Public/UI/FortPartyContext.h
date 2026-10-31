#pragma once

#include "UObject/Class.h"
#include "UObject/Object.h"

class UFortUITeamInfo;

// The local player's party as the front end's UI reads it (a UBlueprintContextBase).
class UFortPartyContext : public UObject
{
    DefineUnrealClass(UFortPartyContext, FortniteGame)
    DefineUFunction(GetLocalPlayerTeam)
    DefineUFunction(GetLocalPlayerTeamMemberInfo)
    DefineUFunction(GetPartyMemberDisplayState)

public:
    // The context the UI's UBlueprintContextLibrary::GetContext gives a widget of this local player.
    static UFortPartyContext* Get(UObject* ContextObject);

    UFortUITeamInfo* GetLocalPlayerTeam();

    // Fills 1.8.1's Battle Royale party bar (FullPartyBarAthena) in the front end, and a mission's escape menu and HUD
    // trackers of the local player (their slot and homebase power), from the game thread's tick.
    static void StartAthenaPartyBar();
};
