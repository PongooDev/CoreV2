#include "pch.h"
#include "Online/FortLobbyBeaconHost.h"
#include "Natives.h"

void AFortLobbyBeaconHost::SetWorldRecordLoaded(bool bInWorldRecordLoaded)
{
    Natives::AFortLobbyBeaconHost_SetWorldRecordLoaded(this, bInWorldRecordLoaded);
}
