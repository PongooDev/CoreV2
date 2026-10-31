#include "pch.h"
#include "Online/FortMcpProfileAccount.h"
#include "Finder.h"
#include "Natives.h"
#include "Online/FortMcpProfileWorld.h"
#include "Quests/FortQuestManager.h"
#include "ReflectionParams.h"
#include "UObject/Class.h"

void UFortMcpProfileAccount::EarnScore(const FString& MatchmakingSessionId, const TArray<FAccountIdAndScore>& PlayersAndScores)
{
    Reflection::FParams Params(StaticFunction_EarnScore(), true);
    Params.Set(L"MatchmakingSessionId", MatchmakingSessionId);
    Params.Set(L"PlayersAndScores", PlayersAndScores);
    Params.Call(this);
}

bool UFortMcpProfileAccount::EndPrimaryMission(FScriptArray& Advance, const FString& MatchmakingSessionId, const FString& MissionName, const FString& MissionStatus, TStructArray<FFortQuestEarnedBadgeData>& EarnedBadgeData, int32_t NumMissionPoints, bool bCriticalMatchBonus)
{
    Reflection::FParams Params(StaticFunction_EndPrimaryMission(), true);
    FScriptArray* AdvanceParameter = Params.Get<FScriptArray>(L"Advance");
    TStructArray<FFortQuestEarnedBadgeData>* BadgesParameter = Params.Get<TStructArray<FFortQuestEarnedBadgeData>>(L"EarnedBadgeData");
    FDedicatedServerUrlContext* Context = Params.Get<FDedicatedServerUrlContext>(L"Context");
    if (!AdvanceParameter || !BadgesParameter || !Context)
    {
        return false;
    }

    // The parameters are destroyed with the command, so the progress moves in and the badges are copied.
    std::swap(*AdvanceParameter, Advance);
    for (int32_t Index = 0; Index < EarnedBadgeData.Num(); ++Index)
    {
        FFortQuestEarnedBadgeData::StaticStruct()->CopyScriptStruct(&BadgesParameter->AddDefaulted(), &EarnedBadgeData[Index]);
    }
    Params.Set(L"MatchmakingSessionId", MatchmakingSessionId);
    Params.Set(L"MissionName", MissionName);
    Params.Set(L"MissionStatus", MissionStatus);
    Params.Set(L"NumMissionPoints", NumMissionPoints);
    Params.Set(L"bCriticalMatchBonus", bCriticalMatchBonus);
    if (Finder::ResolveOutpostMcpRequestLive())
    {
        Context->Bind(nullptr, nullptr);
    }
    return Params.Call(this);
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
