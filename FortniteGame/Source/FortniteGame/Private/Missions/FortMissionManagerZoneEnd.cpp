#include "pch.h"
#include "Missions/FortMissionManager.h"
#include "BlueprintGameplayTagLibrary.h"
#include "Engine/World.h"
#include "Finder.h"
#include "FortGameModeZone.h"
#include "FortGameState.h"
#include "FortWorldManager.h"
#include "GameFramework/PlayerState.h"
#include "Hooks.h"
#include "Items/FortBadgeItemDefinition.h"
#include "Log.h"
#include "Missions/FortMission.h"
#include "Missions/FortMissionLibrary.h"
#include "Natives.h"
#include "Player/FortPlayerController.h"
#include "Player/FortPlayerState.h"
#include "Quests/FortQuestManager.h"
#include "Templates/Casts.h"
#include "Templates/SharedPointer.h"
#include "UObject/Class.h"
#include "UObject/CoreOnline.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace
{
    FString GetGameSessionId(UWorld* World)
    {
        UGameInstance* GameInstance = World ? static_cast<UGameInstance*>(World->OwningGameInstance) : nullptr;
        TSharedPtr<const FUniqueNetId> SessionId;
        if (GameInstance)
        {
            Natives::UGameInstance_GetGameSessionId(GameInstance, &SessionId);
        }
        return SessionId.IsValid() && SessionId->IsValid() ? SessionId->ToString() : FString();
    }

    FString GetMissionStatusName(EFortMissionStatus Status)
    {
        UObject* StatusEnum = Natives::StaticFindObject(nullptr, nullptr, L"/Script/FortniteGame.EFortMissionStatus", false);
        FString Name;
        if (StatusEnum)
        {
            Natives::UEnum_GetEnumName(StatusEnum, &Name, static_cast<int32_t>(Status));
        }
        return Name;
    }
}

void AFortMissionManager::OnZoneEnd(bool bZoneCompleted)
{
#if WITH_SERVER_CODE
    if (Role != ROLE_Authority || !Natives::UGameInstance_GetGameSessionId || !Natives::UEnum_GetEnumName)
    {
        return;
    }

    UWorld* World = GetWorld();
    const FString SessionId = GetGameSessionId(World);
    TArray<AFortMission*>& MissionList = Missions;
    for (AFortMission* Mission : MissionList)
    {
        const EFortMissionStatus Status = Mission ? static_cast<EFortMissionStatus>(Mission->MissionStatus) : EFortMissionStatus::Max_None;
        if (!Mission || !Mission->IsPrimaryMission() || (Status != EFortMissionStatus::Succeeded && Status != EFortMissionStatus::Failed))
        {
            continue;
        }

        UFortMissionInfo* Info = Mission->MissionInfo;
        const int32_t MaxMissionPoints = Info ? static_cast<int32_t>(Info->MaxMissionPoints) : 0;
        const FString MissionName = Info ? Info->GetName() : FString(L"None");
        const FString StatusName = GetMissionStatusName(Status);
        for (AFortPlayerController* PlayerController : Mission->GetParticipatingMissionControllers())
        {
            if (!PlayerController)
            {
                continue;
            }

            AddMissionRewardActivity(PlayerController, Status == EFortMissionStatus::Succeeded);
            if (AFortPlayerState* PlayerState = Cast<AFortPlayerState>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(PlayerController->PlayerState))))
            {
                PlayerState->bAreZoneStatsFinalized = true;
                PlayerState->bIsFinalizingExperience = true;
                PlayerState->ForceNetUpdate();
            }
            PlayerController->ForceNetUpdate();

            UFortQuestManager* QuestManager = PlayerController->GetQuestManager();
            if (!QuestManager)
            {
                continue;
            }

            if (Status == EFortMissionStatus::Succeeded && Info)
            {
                FGameplayTagContainer SourceTags;
                FGameplayTagContainer ContextTags;
                QuestManager->GetSourceAndContextTags(SourceTags, ContextTags);
                FGameplayTagContainer TargetTags = Info->MissionTags;
                UBlueprintGameplayTagLibrary::AppendGameplayTagContainers(TargetTags, Mission->MissionCompletionTags);
                QuestManager->SendStatEvent(EFortQuestObjectiveStatEvent::Complete, Mission, TargetTags, SourceTags, ContextTags, 1, true);
            }

            if (!SessionId.IsEmpty())
            {
                QuestManager->SendEndPrimaryMission(SessionId, MissionName, StatusName, GetMissionPointsForPlayer(PlayerController, MaxMissionPoints), PlayerController->bShouldReceiveCriticalMatchBonus);
            }
        }
    }

    if (AFortGameMode* FortGameMode = World ? Cast<AFortGameMode>(reinterpret_cast<UObject*>(World->GetAuthGameMode())) : nullptr)
    {
        FortGameMode->SetBadgeScores();
    }
    UFortMissionLibrary::ExecuteMissionEvent(this, FName(L"EndOfZone", FNAME_Find), nullptr);

    if (AFortGameStateZone* GameState = World ? Cast<AFortGameStateZone>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr)
    {
        GameState->SetGameplayState(2);
    }
    if (AFortGameModeZone* GameMode = World ? Cast<AFortGameModeZone>(reinterpret_cast<UObject*>(World->GetAuthGameMode())) : nullptr)
    {
        GameMode->OnZoneEnd();
    }
#endif
}

namespace
{
    bool IsSamePlayer(const FUniqueNetIdRepl& A, const FUniqueNetIdRepl& B)
    {
        return A.IsValid() && B.IsValid() && A.ToString().ToStdString() == B.ToString().ToStdString();
    }

    void AddRewardItem(TStructArray<FFortItemEntry>& Items, UFortItemDefinition* ItemDefinition, int32_t Count)
    {
        const int32_t ElementSize = TStructArray<FFortItemEntry>::ElementSize();
        FFortItemEntry* Entry = &Items[Items.Add(1, ElementSize)];
        std::memset(Entry, 0, ElementSize);
        FFortItemEntry::Construct(Entry, ItemDefinition, Count, 0);
    }
}

void AFortMissionManager::AddMissionRewardActivity(AFortPlayerController* PlayerController, bool bSucceeded)
{
#if WITH_SERVER_CODE
    APlayerState* PlayerState = PlayerController->PlayerState;
    if (!PlayerState)
    {
        return;
    }

    FFortRewardReport& Report = PlayerController->LatestRewardReport;
    TStructArray<FFortRewardActivity>& Activities = Report.RewardActivities;
    FFortRewardActivity* Activity = nullptr;
    for (int32_t Index = 0; Index < Activities.Num() && !Activity; ++Index)
    {
        Activity = static_cast<EFortRewardActivityType>(Activities[Index].ActivityType) == EFortRewardActivityType::MissionPrimary ? &Activities[Index] : nullptr;
    }
    if (!Activity)
    {
        Activity = &Activities.AddDefaulted();
        Activity->ActivityType = EFortRewardActivityType::MissionPrimary;
        Activity->ActivityGuid = FGuid::NewGuid();
        Activity->RewardDisplayTime = -1.0f;
    }
    Activity->ActivityCompletionResult = bSucceeded ? EFortCompletionResult::Win : EFortCompletionResult::Loss;

    const FUniqueNetIdRepl& PlayerId = PlayerState->UniqueId;
    FEarnedBadgeEntryArray& BadgeArray = EarnedBadgesArray;
    TStructArray<FEarnedBadgeEntry>& Entries = BadgeArray.Items;
    for (int32_t Index = 0; Index < Entries.Num(); ++Index)
    {
        FEarnedBadgeEntry& Entry = Entries[Index];
        UFortBadgeItemDefinition* Badge = Entry.Badge;
        TStructArray<FEarnedBadgePlayerData>& PlayerData = Entry.PlayerData;
        for (int32_t DataIndex = 0; Badge && DataIndex < PlayerData.Num(); ++DataIndex)
        {
            if (IsSamePlayer(PlayerData[DataIndex].PlayerID, PlayerId))
            {
                const bool bMissed = static_cast<EFortRewardType>(Entry.RewardType) == EFortRewardType::Missed;
                AddRewardItem(bMissed ? static_cast<TStructArray<FFortItemEntry>&>(Activity->MissedRewardItems) : static_cast<TStructArray<FFortItemEntry>&>(Activity->RewardItems), Badge, PlayerData[DataIndex].Count);
            }
        }
    }
    Report.bIsFinalized = true;
#endif
}

void AFortMissionManager::GrantBadge(UFortBadgeItemDefinition* Badge, EFortRewardType RewardType, const TArray<AFortPlayerController*>& Players)
{
#if WITH_SERVER_CODE
    if (Role != ROLE_Authority || !Badge)
    {
        return;
    }

    FEarnedBadgeEntryArray& BadgeArray = EarnedBadgesArray;
    TStructArray<FEarnedBadgeEntry>& Entries = BadgeArray.Items;
    FEarnedBadgeEntry* Entry = nullptr;
    for (int32_t Index = 0; Index < Entries.Num() && !Entry; ++Index)
    {
        FEarnedBadgeEntry& Candidate = Entries[Index];
        Entry = static_cast<UFortBadgeItemDefinition*>(Candidate.Badge) == Badge && static_cast<EFortRewardType>(Candidate.RewardType) == RewardType ? &Candidate : nullptr;
    }
    if (!Entry)
    {
        Entry = &Entries.AddDefaulted();
        Entry->Badge = Badge;
        Entry->RewardType = RewardType;
    }

    UScriptStruct* PlayerIdStruct = static_cast<UStructProperty*>(FEarnedBadgePlayerData::StaticStruct()->FindPropertyByName(FName(L"PlayerID", FNAME_Find)))->Struct;
    TStructArray<FEarnedBadgePlayerData>& PlayerData = Entry->PlayerData;
    for (AFortPlayerController* PlayerController : Players)
    {
        APlayerState* PlayerState = PlayerController ? static_cast<APlayerState*>(PlayerController->PlayerState) : nullptr;
        if (!PlayerState)
        {
            continue;
        }

        const FUniqueNetIdRepl& PlayerId = PlayerState->UniqueId;
        FEarnedBadgePlayerData* Data = nullptr;
        for (int32_t Index = 0; Index < PlayerData.Num() && !Data; ++Index)
        {
            Data = IsSamePlayer(PlayerData[Index].PlayerID, PlayerId) ? &PlayerData[Index] : nullptr;
        }
        if (!Data)
        {
            Data = &PlayerData.AddDefaulted();
            PlayerIdStruct->CopyScriptStruct(&static_cast<FUniqueNetIdRepl&>(Data->PlayerID), &PlayerId);
        }
        Data->Count = Data->Count + 1;
    }
    BadgeArray.MarkItemDirty(*Entry);
#endif
}

int32_t AFortMissionManager::GetMissionPointsForPlayer(AFortPlayerController* PlayerController, int32_t MaxMissionPoints)
{
    APlayerState* PlayerState = PlayerController ? static_cast<APlayerState*>(PlayerController->PlayerState) : nullptr;
    if (!PlayerState)
    {
        return 0;
    }

    int32_t Points = 0;
    const FUniqueNetIdRepl& PlayerId = PlayerState->UniqueId;
    FEarnedBadgeEntryArray& BadgeArray = EarnedBadgesArray;
    TStructArray<FEarnedBadgeEntry>& Entries = BadgeArray.Items;
    for (int32_t Index = 0; Index < Entries.Num(); ++Index)
    {
        FEarnedBadgeEntry& Entry = Entries[Index];
        UFortBadgeItemDefinition* Badge = Entry.Badge;
        if (!Badge || static_cast<EFortRewardType>(Entry.RewardType) != EFortRewardType::Default)
        {
            continue;
        }
        TStructArray<FEarnedBadgePlayerData>& PlayerData = Entry.PlayerData;
        for (int32_t DataIndex = 0; DataIndex < PlayerData.Num(); ++DataIndex)
        {
            if (IsSamePlayer(PlayerData[DataIndex].PlayerID, PlayerId))
            {
                Points += Badge->GetMissionPoints() * PlayerData[DataIndex].Count;
            }
        }
    }
    return MaxMissionPoints > 0 ? std::min(Points, MaxMissionPoints) : Points;
}

void AFortMissionManager::HookVTables()
{
    if (!Finder::ResolveZoneEndLive(AFortMission::MissionInfoOffset(), AFortWorldManager::ZoneFinishedOffset(), AFortWorldManager::ZoneCompletedOffset(), AFortGameModeZone::EndOfZoneRemainTimeOffset(), AFortGameModeZone::ActiveSpawnPadOffset()))
    {
        return;
    }

    const int32_t Index = Natives::AFortMissionManager_OnZoneEndIndex;
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && VTable[Index] == Natives::SharedEmptyFunction)
        {
            Hooks::SwapVTable(L"AFortMissionManager::OnZoneEnd", VTable, Index, reinterpret_cast<void*>(&OnZoneEndHK));
        }
    }
}

void AFortMissionManager::OnZoneEndHK(AFortMissionManager* This, bool bZoneCompleted)
{
    This->OnZoneEnd(bZoneCompleted);
}
