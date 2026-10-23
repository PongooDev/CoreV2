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

struct FFortSavedModeLoadout
{
    FString LoadoutName;
    TArray<FString> SelectedGadgets;
};

static_assert(sizeof(FFortSavedModeLoadout) == 0x20);

class UFortMcpProfileAccount : public UObject
{
    DefineUnrealClass(UFortMcpProfileAccount, FortniteGame)
    DefineUFunction(EarnScore)

public:
    void EarnScore(const FString& MatchmakingSessionId, const TArray<FAccountIdAndScore>& PlayersAndScores);
    FFortSavedModeLoadout GetModeLoadout(const FString& LoadoutName) const;
};
