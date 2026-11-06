#include "pch.h"
#include "Online/FortMcpProfileCampaign.h"
#include "Finder.h"
#include "Natives.h"
#include "Online/FortMcpProfileWorld.h"
#include "Quests/FortQuestManager.h"
#include "ReflectionParams.h"
#include "UObject/Class.h"

// The MCP commands are found, not required: a build without one has the caller report it, where a missing function
// stopped the server at the end of every mission (#279).
bool UFortMcpProfileCampaign::EarnScore(const FString& MatchmakingSessionId, const TArray<FAccountIdAndScore>& PlayersAndScores)
{
    Reflection::FParams Params(FindStaticFunction_EarnScore(), true);
    Params.Set(L"MatchmakingSessionId", MatchmakingSessionId);
    Params.Set(L"PlayersAndScores", PlayersAndScores);
    return Params.Call(this);
}

bool UFortMcpProfileCampaign::EndPrimaryMission(FScriptArray& Advance, const FString& MatchmakingSessionId, const FString& MissionName, const FString& MissionStatus, TStructArray<FFortQuestEarnedBadgeData>& EarnedBadgeData, const TArray<FString>& EarnedItemCaches, int32_t DifficultyIncreaseTier, int32_t NumMissionPoints, bool bCriticalMatchBonus)
{
    Reflection::FParams Params(FindStaticFunction_EndPrimaryMission(), true);
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
    // The BluGlo Difficulty Pylon's tier (3.0's EndPrimaryMission has it between EarnedBadgeData and NumMissionPoints);
    // builds before the pylon have no such parameter.
    if (Params.Has(L"DifficultyIncreaseTier"))
    {
        Params.Set(L"DifficultyIncreaseTier", DifficultyIncreaseTier);
    }
    // The item caches the player picked up (4.0's TArray<FString> EarnedItemCaches, between EarnedBadgeData and
    // DifficultyIncreaseTier); builds before item caches have no such parameter.
    if (TArray<FString>* CachesParameter = Params.Has(L"EarnedItemCaches") ? Params.Get<TArray<FString>>(L"EarnedItemCaches") : nullptr)
    {
        for (const FString& ItemCache : EarnedItemCaches)
        {
            CachesParameter->Add(ItemCache);
        }
    }
    Params.Set(L"NumMissionPoints", NumMissionPoints);
    Params.Set(L"bCriticalMatchBonus", bCriticalMatchBonus);
    if (Finder::ResolveOutpostMcpRequestLive())
    {
        Context->Bind(nullptr, nullptr);
    }
    return Params.Call(this);
}

// LoadoutName is taken by value: the game frees its buffer when it returns (1.9.1 0x742410, 1.10 0x7469F0, 1.11
// 0x775150), so it gets its own copy. Passed by reference, Core freed the same 16-byte "Default" block again, and the
// allocator handed it out twice; the next 16-byte allocation crashed on the text written over its free-list link (#175).
FFortSavedModeLoadout UFortMcpProfileCampaign::GetModeLoadout(FString LoadoutName) const
{
    FFortSavedModeLoadout ModeLoadout;
    if (Natives::UFortMcpProfileCampaign_GetModeLoadout)
    {
        Natives::UFortMcpProfileCampaign_GetModeLoadout(this, &ModeLoadout, std::move(LoadoutName));
    }
    return ModeLoadout;
}
