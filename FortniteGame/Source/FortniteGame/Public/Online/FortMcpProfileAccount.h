#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "StructArray.h"
#include "UObject/Object.h"

struct FFortQuestEarnedBadgeData;

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
    DefineUFunction(EndPrimaryMission)

public:
    void EarnScore(const FString& MatchmakingSessionId, const TArray<FAccountIdAndScore>& PlayersAndScores);
    // Sends EndPrimaryMission as the dedicated server. Advance (TArray<FFortQuestObjectiveCompletion>) is moved into the
    // command and left empty.
    bool EndPrimaryMission(FScriptArray& Advance, const FString& MatchmakingSessionId, const FString& MissionName, const FString& MissionStatus, TStructArray<FFortQuestEarnedBadgeData>& EarnedBadgeData, int32_t NumMissionPoints, bool bCriticalMatchBonus);
    FFortSavedModeLoadout GetModeLoadout(const FString& LoadoutName) const;
};
