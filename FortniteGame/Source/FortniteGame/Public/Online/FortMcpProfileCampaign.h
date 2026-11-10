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

// One player's part of EndMatch (5.21, 0x170 bytes): what EndPrimaryMission sent for them, plus the score EarnScore
// sent. The world save's fields after NumMissionPoints (TheaterNum, TheaterItemUpdate, OutpostNum, OutpostItemUpdate,
// OutpostId, OutpostInfo, DeployableBaseItemId, CloudSaveInfo, LockCode) are left as InitializeStruct makes them: the
// server's world saves still go out on their own (BatchUpdatePlayers).
struct FAccountIdAndMatchEndData
{
    DefineUnrealStruct(FAccountIdAndMatchEndData, FortniteGame)
    DefineUProperty(FString, AccountId)
    // TArray<FFortQuestObjectiveCompletion>
    DefineUProperty(FScriptArray, Advance)
    // TArray<FFortQuestEarnedBadgeData>
    DefineUProperty(FScriptArray, EarnedBadgeData)
    DefineUProperty(TArray<FString>, EarnedItemCaches)
    DefineUProperty(int32_t, TotalScore)
    DefineUProperty(bool, bCriticalMatchBonus)
    DefineUProperty(int32_t, NumMissionPoints)
};

struct FFortSavedModeLoadout
{
    FString LoadoutName;
    TArray<FString> SelectedGadgets;
};

static_assert(sizeof(FFortSavedModeLoadout) == 0x20);

class UFortMcpProfileCampaign : public UObject
{
    DefineRenamedUnrealClass(UFortMcpProfileCampaign, UFortMcpProfileAccount, FortniteGame)
    DefineUFunction(EarnScore)
    DefineUFunction(EndPrimaryMission)
    DefineUFunction(EndMatch)

public:
    // Whether the build ends a mission with EndMatch (5.21), which replaced EndPrimaryMission and EarnScore's send.
    static bool HasEndMatch() { return FindStaticFunction_EndMatch() != nullptr; }
    // Sends EarnScore as the dedicated server; false when the build has no such command.
    bool EarnScore(const FString& MatchmakingSessionId, const TArray<FAccountIdAndScore>& PlayersAndScores);
    // Sends EndPrimaryMission as the dedicated server. Advance (TArray<FFortQuestObjectiveCompletion>) is moved into the
    // command and left empty. DifficultyIncreaseTier is sent where the function has it (2.5+), EarnedItemCaches (the
    // template ids of the item caches the player picked up) where it has that (4.0+).
    bool EndPrimaryMission(FScriptArray& Advance, const FString& MatchmakingSessionId, const FString& MissionName, const FString& MissionStatus, TStructArray<FFortQuestEarnedBadgeData>& EarnedBadgeData, const TArray<FString>& EarnedItemCaches, int32_t DifficultyIncreaseTier, int32_t NumMissionPoints, bool bCriticalMatchBonus);
    // Sends EndMatch (5.21) as the dedicated server: the mission's end for each player in PlayersAndMatchEndData, which
    // is moved into the command and left empty. False when the build has no such command.
    bool EndMatch(TStructArray<FAccountIdAndMatchEndData>& PlayersAndMatchEndData, const FString& MatchmakingSessionId, const FString& MissionName, const FString& MissionStatus, int32_t DifficultyIncreaseTier);
    FFortSavedModeLoadout GetModeLoadout(FString LoadoutName) const;
};
