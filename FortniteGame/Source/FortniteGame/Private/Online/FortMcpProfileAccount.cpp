#include "pch.h"
#include "Online/FortMcpProfileAccount.h"
#include "Natives.h"
#include "ReflectionParams.h"
#include "UObject/Class.h"

void UFortMcpProfileAccount::EarnScore(const FString& MatchmakingSessionId, const TArray<FAccountIdAndScore>& PlayersAndScores)
{
    Reflection::FParams Params(StaticFunction_EarnScore(), true);
    Params.Set(L"MatchmakingSessionId", MatchmakingSessionId);
    Params.Set(L"PlayersAndScores", PlayersAndScores);
    Params.Call(this);
}

FFortSavedModeLoadout UFortMcpProfileAccount::GetModeLoadout(const FString& LoadoutName) const
{
    FFortSavedModeLoadout ModeLoadout;
    if (Natives::UFortMcpProfileAccount_GetModeLoadout)
    {
        Natives::UFortMcpProfileAccount_GetModeLoadout(this, &ModeLoadout, LoadoutName);
    }
    return ModeLoadout;
}
