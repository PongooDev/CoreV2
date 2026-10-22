#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "UObject/Object.h"

struct FAccountIdAndScore
{
    FString AccountId;
    int32_t TotalScore = 0;
    bool bCriticalMatchBonus = false;
};

static_assert(sizeof(FAccountIdAndScore) == 0x18);

class UFortMcpProfileAccount : public UObject
{
    DefineUnrealClass(UFortMcpProfileAccount, FortniteGame)
    DefineUFunction(EarnScore)

public:
    void EarnScore(const FString& MatchmakingSessionId, const TArray<FAccountIdAndScore>& PlayersAndScores);
};
