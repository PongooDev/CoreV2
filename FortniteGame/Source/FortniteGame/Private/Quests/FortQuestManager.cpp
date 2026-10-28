#include "pch.h"
#include "Quests/FortQuestManager.h"
#include "Athena/FortGameModeAthena.h"
#include "Containers/ScriptArray.h"
#include "Engine/World.h"
#include "Finder.h"
#include "GameThread.h"
#include "Hooks.h"
#include "Items/FortItemDefinition.h"
#include "Natives.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/WeakObjectPtr.h"

#include <algorithm>
#include <chrono>
#include <utility>
#include <vector>

namespace
{
    void (*SendStatEventWithTagsOG)(UFortQuestManager* This, const void* CustomStatName, uint8_t Type, UObject* TargetObject, const FGameplayTagContainer& TargetTags, const FGameplayTagContainer& SourceTags, const FGameplayTagContainer& ContextTags, int32_t Count, bool bFlush) = nullptr;

    // Battle Royale quest managers holding progress from events that asked for no flush, and when it is saved.
    std::vector<std::pair<FWeakObjectPtr, std::chrono::steady_clock::time_point>> DueSaves;
    constexpr auto SaveDelay = std::chrono::seconds(5);

    // The quest manager's flush: its pending progress goes to its profile as UpdateQuests, nothing is sent when none is
    // pending. The completion delegate it takes by value is left unbound.
    void SaveProgress(UFortQuestManager* QuestManager)
    {
        alignas(16) uint8_t OnComplete[0x40] = {};
        Natives::UFortQuestManager_FlushStatEvents(QuestManager, OnComplete);
    }

    void SaveWhenDue()
    {
        GameThread::Enqueue([]
        {
            const auto Now = std::chrono::steady_clock::now();
            std::erase_if(DueSaves, [Now](const auto& Due)
            {
                if (Due.second > Now)
                {
                    return false;
                }
                if (UFortQuestManager* QuestManager = static_cast<UFortQuestManager*>(Due.first.Get()))
                {
                    SaveProgress(QuestManager);
                }
                return true;
            });
            if (!DueSaves.empty())
            {
                SaveWhenDue();
            }
        });
    }
}

void UFortQuestManager::GetSourceAndContextTags(FGameplayTagContainer& OutSourceTags, FGameplayTagContainer& OutContextTags)
{
    Reflection::FParams Params(StaticFunction_GetSourceAndContextTags(), true);
    if (Params.Call(this))
    {
        OutSourceTags = Params.Take<FGameplayTagContainer>(L"OutSourceTags");
        OutContextTags = Params.Take<FGameplayTagContainer>(L"OutContextTags");
    }
}

UFortQuestItem* UFortQuestManager::GetQuestWithDefinition(UFortQuestItemDefinition* Definition)
{
    Reflection::FParams Params(StaticFunction_GetQuestWithDefinition());
    Params.Set(L"Definition", Definition);
    return Params.Call(this) ? Params.Take<UFortQuestItem*>(L"ReturnValue") : nullptr;
}

bool UFortQuestManager::HasActiveQuest(UFortQuestItemDefinition* Definition)
{
    UFortQuestItem* Quest = Definition ? GetQuestWithDefinition(Definition) : nullptr;
    return Quest && Quest->quest_state == EFortQuestState::GetActive();
}

void UFortQuestManager::SendStatEvent(EFortQuestObjectiveStatEvent Type, UObject* TargetObject, const FGameplayTagContainer& TargetTags, const FGameplayTagContainer& SourceTags, const FGameplayTagContainer& ContextTags, int32_t Count, bool bFlush)
{
    if (Natives::UFortQuestManager_SendStatEvent)
    {
        Natives::UFortQuestManager_SendStatEvent(this, static_cast<uint8_t>(Type), TargetObject, TargetTags, SourceTags, ContextTags, Count, bFlush);
    }
}

// Collect and Craft events name the item definition, which item-event objectives (ItemReference) and stat rows with
// Item.* conditions are matched against.
void UFortQuestManager::SendItemStatEvent(EFortQuestObjectiveStatEvent Type, UFortItemDefinition* ItemDefinition, int32_t Count)
{
#if WITH_SERVER_CODE
    if (!ItemDefinition || Count <= 0)
    {
        return;
    }

    FGameplayTagContainer SourceTags;
    FGameplayTagContainer ContextTags;
    GetSourceAndContextTags(SourceTags, ContextTags);
    SendStatEvent(Type, ItemDefinition, ItemDefinition->GameplayTags, SourceTags, ContextTags, Count, false);
#endif
}

void UFortQuestManager::SendEndPrimaryMission(const FString& MatchmakingSessionId, const FString& MissionName, const FString& MissionStatus, TStructArray<FFortQuestEarnedBadgeData>& EarnedBadges, int32_t NumMissionPoints, bool bCriticalMatchBonus)
{
    if (Natives::UFortQuestManager_SendEndPrimaryMission)
    {
        Natives::UFortQuestManager_SendEndPrimaryMission(this, MatchmakingSessionId, MissionName, MissionStatus, EarnedBadges, NumMissionPoints, bCriticalMatchBonus);
    }
}

void UFortQuestManager::Hook()
{
    if (Finder::ResolveQuestStatEvents())
    {
        Hooks::Detour(L"UFortQuestManager::SendStatEventWithTags", reinterpret_cast<void*>(Natives::UFortQuestManager_SendStatEventWithTags), &SendStatEventWithTagsHK, &SendStatEventWithTagsOG);
    }
}

void UFortQuestManager::SendStatEventWithTagsHK(UFortQuestManager* This, const void* CustomStatName, uint8_t Type, UObject* TargetObject, const FGameplayTagContainer& TargetTags, const FGameplayTagContainer& SourceTags, const FGameplayTagContainer& ContextTags, int32_t Count, bool bFlush)
{
#if WITH_SERVER_CODE
    // Battle Royale's challenges (1.8), Core-Backend #92: a chest search (ServerAttemptInteract, 0x845020: an Interact
    // event with a flush) never reached the athena profile, for two reasons.
    // First, the event's context is the game mode's GameContextTags, which nothing fills in Battle Royale, and the
    // objectives ask for the playlist there (AFortGameModeAthena::AddPlaylistContextTag): the game mode gets it, and so
    // does the event, whose tags were copied from it before.
    UFortRegisteredPlayerInfo* PlayerInfo = Cast<UFortRegisteredPlayerInfo>(This ? This->GetOuter() : nullptr);
    AFortGameModeAthena* GameMode = PlayerInfo && PlayerInfo->GetAthenaQuestManager() == This ? AFortGameModeAthena::GetCurrent(GWorld) : nullptr;
    if (!GameMode)
    {
        SendStatEventWithTagsOG(This, CustomStatName, Type, TargetObject, TargetTags, SourceTags, ContextTags, Count, bFlush);
        return;
    }
    GameMode->AddPlaylistContextTag(static_cast<FGameplayTagContainer&>(GameMode->GameContextTags));
    FGameplayTagContainer EventContextTags = ContextTags;
    GameMode->AddPlaylistContextTag(EventContextTags);
    SendStatEventWithTagsOG(This, CustomStatName, Type, TargetObject, TargetTags, SourceTags, EventContextTags, Count, bFlush);

    // Second, SendStatEventWithTags (0x890730) adds what the event advanced to the quest manager's pending progress
    // (+0x128) and, when the event asked for a flush or completed an objective, saves it, but not for an
    // AFortPlayerControllerAthena (0x891500-0x891565). Nothing else saves it: its flush (0x88E060) has no other caller,
    // and UFortMcpProfileAthena::EndBattleRoyaleGame, which takes the same progress as Advance, has no caller left (its
    // FName, 0x662E1A0, is only made). Epic's servers took Battle Royale's challenges from its stats instead. The
    // athena quest manager's progress is saved here, at once when the event asked for it, as Save the World's is, and
    // within a few seconds otherwise, so bursts (every other player's death is an AthenaOutlive event, 0x599DA0,
    // unflushed) make one UpdateQuests.
    if (bFlush)
    {
        SaveProgress(This);
        return;
    }
    const FWeakObjectPtr Weak(This);
    if (std::none_of(DueSaves.begin(), DueSaves.end(), [&Weak](const auto& Due) { return Due.first == Weak; }))
    {
        const bool bIdle = DueSaves.empty();
        DueSaves.emplace_back(Weak, std::chrono::steady_clock::now() + SaveDelay);
        if (bIdle)
        {
            SaveWhenDue();
        }
    }
#else
    SendStatEventWithTagsOG(This, CustomStatName, Type, TargetObject, TargetTags, SourceTags, ContextTags, Count, bFlush);
#endif
}
