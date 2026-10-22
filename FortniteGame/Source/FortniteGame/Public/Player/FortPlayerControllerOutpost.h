#pragma once

#include "Player/FortPlayerControllerZone.h"

class AFortPlayerControllerOutpost : public AFortPlayerControllerZone
{
    DefineUnrealClass(AFortPlayerControllerOutpost, FortniteGame)
    DefineUFunction(ServerLeaveOutpostAsParty)

public:
    void ServerLeaveOutpostAsParty_Implementation();

    static void HookVTables();

private:
    static void ServerLeaveOutpostAsParty_ImplementationHK(AFortPlayerControllerOutpost* This);
};
