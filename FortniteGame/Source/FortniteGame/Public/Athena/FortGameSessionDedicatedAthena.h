#pragma once

#include "Online/FortGameSessionDedicated.h"

// Battle Royale's dedicated game session (from 5.0, CL-4204761). 5.0 moved the matchmaking service registration out of
// AFortGameSessionDedicated into it: its MMSVersionCompatability, MMSTicketURLServer and MMSPingInterval, and the
// registration the session's creation starts (CL-4204761 0xFB5FA0, from slot 246, 0xFC1580). Builds before 5.0 don't
// have it, and StaticClass is null there.
class AFortGameSessionDedicatedAthena : public AFortGameSessionDedicated
{
    DefineUnrealClass(AFortGameSessionDedicatedAthena, FortniteGame)
};
