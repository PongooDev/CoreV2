#include "pch.h"
#include "Online/FortMcpProfileAccount.h"
#include "ReflectionParams.h"
#include "UObject/Class.h"

void UFortMcpProfileAccount::EarnScore(const FString& MatchmakingSessionId, const TArray<FAccountIdAndScore>& PlayersAndScores)
{
    Reflection::FParams Params(StaticFunction_EarnScore(), true);
    Params.Set(L"MatchmakingSessionId", MatchmakingSessionId);
    Params.Set(L"PlayersAndScores", PlayersAndScores);
    Params.Call(this);
}
