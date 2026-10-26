#include "pch.h"
#include "Missions/FortMissionManager.h"
#include "BlueprintGameplayTagLibrary.h"
#include "Engine/World.h"
#include "Finder.h"
#include "FortGameModeZone.h"
#include "FortGameState.h"
#include "FortKismetLibrary.h"
#include "FortWorldManager.h"
#include "GameFramework/PlayerState.h"
#include "Hooks.h"
#include "Items/FortBadgeItemDefinition.h"
#include "Items/FortInventory.h"
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
        UEnum* StatusEnum = EFortMissionStatus::StaticEnum();
        FString Name;
        if (StatusEnum && Natives::UEnum_GetEnumName)
        {
            Natives::UEnum_GetEnumName(StatusEnum, &Name, static_cast<int32_t>(Status));
        }
        return Name;
    }

    // "<badge> x<count> (<mission points> MP)" for each badge in a report activity's items. The results screen's chest
    // (Results_BadgeLoot) shows only badges whose BadgeScoring MissionPoints are above 0, and its loot level is the
    // rewarded badges' MissionPoints / 5 + 1 (Results_Summary).
    std::wstring DescribeBadges(TStructArray<FFortItemEntry>& Items)
    {
        std::wstring Text;
        for (int32_t Index = 0; Index < Items.Num(); ++Index)
        {
            UFortItemDefinition* Definition = Items[Index].ItemDefinition;
            UFortBadgeItemDefinition* Badge = Cast<UFortBadgeItemDefinition>(reinterpret_cast<UObject*>(Definition));
            Text += std::format(L"{}{} x{} ({} MP)", Text.empty() ? L"" : L", ", Definition ? Definition->GetName().ToStdString() : std::wstring(L"None"), static_cast<int32_t>(Items[Index].Count), Badge ? Badge->GetMissionPoints() : 0);
        }
        return Text.empty() ? std::wstring(L"none") : Text;
    }

    // The Complete stat for a won mission: its MissionTags plus the completion tags its objectives added
    // (e.g. Stat.Mission.Primary.Outpost.Leg1), which the ObjectiveStatTable's Complete rows target.
    void SendMissionCompleteStat(UFortQuestManager* QuestManager, AFortMission* Mission, UFortMissionInfo* Info)
    {
        FGameplayTagContainer SourceTags;
        FGameplayTagContainer ContextTags;
        QuestManager->GetSourceAndContextTags(SourceTags, ContextTags);
        FGameplayTagContainer TargetTags = Info->MissionTags;
        UBlueprintGameplayTagLibrary::AppendGameplayTagContainers(TargetTags, Mission->MissionCompletionTags);
        QuestManager->SendStatEvent(EFortQuestObjectiveStatEvent::GetComplete(), Mission, TargetTags, SourceTags, ContextTags, 1, true);
    }
}

void AFortMissionManager::OnZoneEnd(bool bZoneCompleted)
{
#if WITH_SERVER_CODE
    if (Role != ROLE_Authority || !Natives::UGameInstance_GetGameSessionId || !Natives::UEnum_GetEnumName)
    {
        return;
    }

    Log::Info(L"AFortMissionManager::OnZoneEnd: zone completed {}", bZoneCompleted);
    UWorld* World = GetWorld();
    const FString SessionId = GetGameSessionId(World);
    // EndPrimaryMission is sent after EarnScore (see below), so the players' commands wait here.
    struct FPendingEndPrimaryMission
    {
        UFortQuestManager* QuestManager;
        FString MissionName;
        FString StatusName;
        TStructArray<FFortQuestEarnedBadgeData> EarnedBadges;
        int32_t MissionPoints;
        bool bCriticalMatchBonus;
    };
    std::vector<FPendingEndPrimaryMission> PendingEndPrimaryMissions;
    TArray<AFortMission*>& MissionList = Missions;
    for (AFortMission* Mission : MissionList)
    {
        const EFortMissionStatus Status = Mission ? Mission->MissionStatus : EFortMissionStatus::GetMax_None();
        if (!Mission || !Mission->IsPrimaryMission() || (Status != EFortMissionStatus::GetSucceeded() && Status != EFortMissionStatus::GetFailed()))
        {
            continue;
        }

        SetMissionCompletionResult(Mission);
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

            SyncMissionRewardItems(PlayerController);
            FFortRewardReport& Report = PlayerController->LatestRewardReport;
            Report.bIsFinalized = true;
            PlayerController->ForceNetUpdate();
            AFortPlayerState* PlayerState = Cast<AFortPlayerState>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(PlayerController->PlayerState)));
            if (PlayerState)
            {
                PlayerState->bAreZoneStatsFinalized = true;
                PlayerState->ForceNetUpdate();
            }

            UFortQuestManager* QuestManager = PlayerController->GetQuestManager();
            if (QuestManager && Status == EFortMissionStatus::GetSucceeded() && Info)
            {
                SendMissionCompleteStat(QuestManager, Mission, Info);
            }

            if (!QuestManager || SessionId.IsEmpty())
            {
                Log::Error(L"OnZoneEnd: no EndPrimaryMission for {} (quest manager {}, session {}), so its experience is never finalized", PlayerController->GetName().ToStdString(), QuestManager != nullptr, SessionId.ToStdString());
                continue;
            }
            if (PlayerState && AFortPlayerState::HasFinalizingExperience())
            {
                PlayerState->bIsFinalizingExperience = true;
            }
            const int32_t MissionPoints = GetMissionPointsForPlayer(PlayerController, MaxMissionPoints);
            TStructArray<FFortRewardActivity>& Activities = Report.RewardActivities;
            for (int32_t Index = 0; Index < Activities.Num(); ++Index)
            {
                FFortRewardActivity& Activity = Activities[Index];
                if (Activity.ActivityType == EFortRewardActivityType::GetMissionPrimary())
                {
                    Log::Info(L"OnZoneEnd: {} badges {}; missed {}; mission points {}", PlayerController->GetName().ToStdString(), DescribeBadges(static_cast<TStructArray<FFortItemEntry>&>(Activity.RewardItems)), DescribeBadges(static_cast<TStructArray<FFortItemEntry>&>(Activity.MissedRewardItems)), MissionPoints);
                }
            }
            FPendingEndPrimaryMission& Pending = PendingEndPrimaryMissions.emplace_back(FPendingEndPrimaryMission{ QuestManager, MissionName, StatusName, {}, MissionPoints, PlayerController->bShouldReceiveCriticalMatchBonus });
            CollectEarnedBadgeData(PlayerController, Pending.EarnedBadges);
        }
    }

    // EarnScore goes out before the EndPrimaryMissions, on the same MCP queue as the sending player's. Each
    // earnScore notification adds an ExperienceDelta and, once bIsFinalizingExperience is set, sets
    // bIsExperienceFinalized (0x762AE0); Results_Widget polls IsDataFinalized and reads the score report's XP once
    // (Results_CommanderXP.Initialize -> GetXpInfo, the sum of the deltas). EndPrimaryMission's earnScore carries no XP,
    // so when it came first the player state could replicate finalized with only that delta and the results screen
    // showed "Total XP Gained 0" (issue #34; the client's LogXPData had every Delta at 0 while the server got the real
    // delta about 100 ms later). EarnScore's own notification now finalizes the sending player with the XP in it.
    // A response's notifications only reach the player whose profile sent the command (the MCP parser 0x3903DB0
    // addresses them to the profile group's account, and the handler is bound per player), so every other player's XP
    // notification comes in the response to their own EndPrimaryMission below (Core-Backend holds it from EarnScore and
    // waits for EarnScore if an EndPrimaryMission gets there first).
    if (AFortGameMode* FortGameMode = World ? Cast<AFortGameMode>(reinterpret_cast<UObject*>(World->GetAuthGameMode())) : nullptr)
    {
        FortGameMode->SetBadgeScores();
        FortGameMode->SendEarnScore();
    }
    for (FPendingEndPrimaryMission& Pending : PendingEndPrimaryMissions)
    {
        Pending.QuestManager->SendEndPrimaryMission(SessionId, Pending.MissionName, Pending.StatusName, Pending.EarnedBadges, Pending.MissionPoints, Pending.bCriticalMatchBonus);
        for (int32_t Index = 0; Index < Pending.EarnedBadges.Num(); ++Index)
        {
            FFortQuestEarnedBadgeData::StaticStruct()->DestroyStruct(&Pending.EarnedBadges[Index]);
        }
        FMemory::Free(Pending.EarnedBadges.GetData());
    }
    UFortMissionLibrary::ExecuteMissionEvent(this, FName(L"EndOfZone", FNAME_Find), nullptr);

    if (AFortGameStateZone* GameState = World ? Cast<AFortGameStateZone>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr)
    {
        GameState->SetGameplayState(EFortGameplayState::GetEndOfZone());
    }
    if (AFortGameModeZone* GameMode = World ? Cast<AFortGameModeZone>(reinterpret_cast<UObject*>(World->GetAuthGameMode())) : nullptr)
    {
        GameMode->OnZoneEnd();
    }
#endif
}

void AFortMissionManager::OnMissionStatusChange(AFortMission* Mission, EFortMissionStatus NewStatus)
{
    OnMissionStatusChangeOG(this, Mission, NewStatus);

#if WITH_SERVER_CODE
    if (Role == ROLE_Authority && Mission)
    {
        Log::Info(L"AFortMissionManager::OnMissionStatusChange: {} is now {}", Mission->GetName().ToStdString(), GetMissionStatusName(NewStatus).ToStdString());
    }
    UFortMissionInfo* Info = Mission ? static_cast<UFortMissionInfo*>(Mission->MissionInfo) : nullptr;
    if (Role == ROLE_Authority && Info && (NewStatus == EFortMissionStatus::GetSucceeded() || (NewStatus == EFortMissionStatus::GetFailed() && Mission->IsPrimaryMission())))
    {
        const bool bSucceeded = NewStatus == EFortMissionStatus::GetSucceeded();
        const TArray<AFortPlayerController*> Players = Mission->GetParticipatingMissionControllers();
        auto GrantMissionBadge = [this, Mission, &Players](UFortBadgeItemDefinition* Badge, bool bEarned)
        {
            if (bEarned)
            {
                Mission->GrantBadge(Badge, EFortRewardType::GetDefault());
            }
            else
            {
                GrantBadge(Badge, EFortRewardType::GetMissed(), Players);
            }
        };
        if (UFortBadgeItemDefinition* RewardBadge = Info->MissionRewardBadge)
        {
            GrantMissionBadge(RewardBadge, bSucceeded);
        }
        UFortBadgeItemDefinition* UnderParBadge = Info->UnderParBadge;
        if (UnderParBadge && Info->ParTime > 0)
        {
            GrantMissionBadge(UnderParBadge, bSucceeded && Mission->IsUnderPar());
        }
    }
    if (Role == ROLE_Authority && Info && NewStatus == EFortMissionStatus::GetSucceeded() && !Mission->IsPrimaryMission())
    {
        for (AFortPlayerController* PlayerController : Mission->GetParticipatingMissionControllers())
        {
            if (UFortQuestManager* QuestManager = PlayerController ? PlayerController->GetQuestManager() : nullptr)
            {
                SendMissionCompleteStat(QuestManager, Mission, Info);
            }
        }
    }
    if (Role == ROLE_Authority && Mission && NewStatus == EFortMissionStatus::GetSucceeded())
    {
        TArray<UFortWorldItemDefinition*> SuccessItems;
        if (Mission->GetAdditionalMissionSuccessBadges(SuccessItems))
        {
            for (UFortWorldItemDefinition* Item : SuccessItems)
            {
                if (UFortBadgeItemDefinition* Badge = Cast<UFortBadgeItemDefinition>(reinterpret_cast<UObject*>(Item)))
                {
                    Mission->GrantBadge(Badge, EFortRewardType::GetDefault());
                }
            }
        }
    }
    if (Role == ROLE_Authority && Mission && Mission->IsPrimaryMission() && (NewStatus == EFortMissionStatus::GetSucceeded() || NewStatus == EFortMissionStatus::GetFailed()))
    {
        SetMissionCompletionResult(Mission);
    }
#endif
}

namespace
{
    bool IsSamePlayer(const FUniqueNetIdRepl& A, const FUniqueNetIdRepl& B)
    {
        return A.IsValid() && B.IsValid() && A.ToString().ToStdString() == B.ToString().ToStdString();
    }

    FFortRewardActivity& FindOrAddMissionPrimaryActivity(AFortPlayerController* PlayerController)
    {
        FFortRewardReport& Report = PlayerController->LatestRewardReport;
        TStructArray<FFortRewardActivity>& Activities = Report.RewardActivities;
        for (int32_t Index = 0; Index < Activities.Num(); ++Index)
        {
            if (Activities[Index].ActivityType == EFortRewardActivityType::GetMissionPrimary())
            {
                return Activities[Index];
            }
        }

        FFortRewardActivity& Activity = Activities.AddDefaulted();
        Activity.ActivityType = EFortRewardActivityType::GetMissionPrimary();
        Activity.ActivityGuid = FGuid::NewGuid();
        Activity.RewardDisplayTime = -1.0f;
        return Activity;
    }

    void SetRewardItemCount(TStructArray<FFortItemEntry>& Items, UFortItemDefinition* ItemDefinition, int32_t Count)
    {
        for (int32_t Index = 0; Index < Items.Num(); ++Index)
        {
            if (static_cast<UFortItemDefinition*>(Items[Index].ItemDefinition) == ItemDefinition)
            {
                Items[Index].Count = Count;
                return;
            }
        }

        const int32_t ElementSize = TStructArray<FFortItemEntry>::ElementSize();
        FFortItemEntry* Entry = &Items[Items.Add(1, ElementSize)];
        std::memset(Entry, 0, ElementSize);
        FFortItemEntry::Construct(Entry, ItemDefinition, Count, 0);
    }
}

void AFortMissionManager::SyncMissionRewardItems(AFortPlayerController* PlayerController)
{
#if WITH_SERVER_CODE
    APlayerState* PlayerState = PlayerController ? static_cast<APlayerState*>(PlayerController->PlayerState) : nullptr;
    if (!PlayerState)
    {
        return;
    }

    FFortRewardActivity& Activity = FindOrAddMissionPrimaryActivity(PlayerController);
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
                const bool bMissed = Entry.RewardType == EFortRewardType::GetMissed();
                SetRewardItemCount(bMissed ? static_cast<TStructArray<FFortItemEntry>&>(Activity.MissedRewardItems) : static_cast<TStructArray<FFortItemEntry>&>(Activity.RewardItems), Badge, PlayerData[DataIndex].Count);
            }
        }
    }
    PlayerController->ForceNetUpdate();
#endif
}

void AFortMissionManager::SetMissionCompletionResult(AFortMission* Mission)
{
#if WITH_SERVER_CODE
    const EFortMissionStatus Status = Mission->MissionStatus;
    const bool bSucceeded = Status == EFortMissionStatus::GetSucceeded();
    const int32_t AdditionalMissionPoints = Mission->GetAdditionalMissionCompletionMissionPoints(Status);
    for (AFortPlayerController* PlayerController : Mission->GetParticipatingMissionControllers())
    {
        if (!PlayerController)
        {
            continue;
        }

        FFortRewardActivity& Activity = FindOrAddMissionPrimaryActivity(PlayerController);
        Activity.ActivityCompletionResult = bSucceeded ? EFortCompletionResult::GetWin() : EFortCompletionResult::GetLoss();
        Activity.AdditionalCompletionMissionPoints = AdditionalMissionPoints;
        PlayerController->ForceNetUpdate();
    }
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
        Entry = static_cast<UFortBadgeItemDefinition*>(Candidate.Badge) == Badge && Candidate.RewardType == RewardType ? &Candidate : nullptr;
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
    Log::Info(L"AFortMissionManager::GrantBadge: {} ({}, {} MP) to {} player(s)", Badge->GetName().ToStdString(), RewardType.ToString(), Badge->GetMissionPoints(), Players.Num());
    for (AFortPlayerController* PlayerController : Players)
    {
        SyncMissionRewardItems(PlayerController);
    }
#endif
}

ptrdiff_t AFortMissionManager::EarnedBadgesArrayOffset()
{
    return Member_EarnedBadgesArray.GetOffsetChecked();
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
        if (!Badge || Entry.RewardType != EFortRewardType::GetDefault())
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
    Points += FindOrAddMissionPrimaryActivity(PlayerController).AdditionalCompletionMissionPoints;
    return MaxMissionPoints > 0 ? std::min(Points, MaxMissionPoints) : Points;
}

// EndPrimaryMission's earnedBadgeData: the player's Default badges in EarnedBadgesArray plus the badges in their world
// inventory (the survivor missions' GrantSurvivorBadge gives them with K2_GiveItemToAllPlayers), which are taken out
// so the theater0 save doesn't keep them.
void AFortMissionManager::CollectEarnedBadgeData(AFortPlayerController* PlayerController, TStructArray<FFortQuestEarnedBadgeData>& OutBadges)
{
#if WITH_SERVER_CODE
    APlayerState* PlayerState = PlayerController ? static_cast<APlayerState*>(PlayerController->PlayerState) : nullptr;
    if (!PlayerState)
    {
        return;
    }

    std::vector<std::pair<UFortBadgeItemDefinition*, int32_t>> Counts;
    auto AddCount = [&Counts](UFortBadgeItemDefinition* Badge, int32_t Count)
    {
        auto It = std::find_if(Counts.begin(), Counts.end(), [Badge](const auto& Pair) { return Pair.first == Badge; });
        if (It == Counts.end())
        {
            Counts.emplace_back(Badge, Count);
        }
        else
        {
            It->second += Count;
        }
    };

    const FUniqueNetIdRepl& PlayerId = PlayerState->UniqueId;
    FEarnedBadgeEntryArray& BadgeArray = EarnedBadgesArray;
    TStructArray<FEarnedBadgeEntry>& Entries = BadgeArray.Items;
    for (int32_t Index = 0; Index < Entries.Num(); ++Index)
    {
        FEarnedBadgeEntry& Entry = Entries[Index];
        UFortBadgeItemDefinition* Badge = Entry.Badge;
        TStructArray<FEarnedBadgePlayerData>& PlayerData = Entry.PlayerData;
        for (int32_t DataIndex = 0; Badge && Entry.RewardType == EFortRewardType::GetDefault() && DataIndex < PlayerData.Num(); ++DataIndex)
        {
            if (IsSamePlayer(PlayerData[DataIndex].PlayerID, PlayerId) && PlayerData[DataIndex].Count > 0)
            {
                AddCount(Badge, PlayerData[DataIndex].Count);
            }
        }
    }

    std::vector<UFortBadgeItemDefinition*> InventoryBadges;
    if (AFortInventory* Inventory = PlayerController->WorldInventory)
    {
        FFortItemList& ItemList = Inventory->Inventory;
        TArray<UFortWorldItem*>& Instances = ItemList.ItemInstances;
        for (int32_t Index = 0; Index < Instances.Num(); ++Index)
        {
            FFortItemEntry& ItemEntry = Instances[Index]->ItemEntry;
            UFortBadgeItemDefinition* Badge = Cast<UFortBadgeItemDefinition>(reinterpret_cast<UObject*>(static_cast<UFortItemDefinition*>(ItemEntry.ItemDefinition)));
            if (Badge && ItemEntry.Count > 0)
            {
                AddCount(Badge, ItemEntry.Count);
                if (std::find(InventoryBadges.begin(), InventoryBadges.end(), Badge) == InventoryBadges.end())
                {
                    InventoryBadges.push_back(Badge);
                }
            }
        }
    }
    for (UFortBadgeItemDefinition* Badge : InventoryBadges)
    {
        UFortKismetLibrary::K2_RemoveItemFromPlayer(PlayerController, Badge, -1);
    }

    std::wstring Text;
    for (const auto& [Badge, Count] : Counts)
    {
        FFortQuestEarnedBadgeData& Data = OutBadges.AddDefaulted();
        Data.TemplateId = Badge->GetPersistentName();
        Data.Count = Count;
        Text += std::format(L"{}{} x{}", Text.empty() ? L"" : L", ", Data.TemplateId.ToStdString(), Count);
    }
    Log::Info(L"OnZoneEnd: {} earnedBadgeData {} ({} badge type(s) from the inventory)", PlayerController->GetName().ToStdString(), Text.empty() ? std::wstring(L"none") : Text, InventoryBadges.size());
#endif
}

void AFortMissionManager::HookVTables()
{
    if (!Finder::ResolveZoneEndLive(AFortMission::MissionInfoOffset(), AFortWorldManager::ZoneFinishedOffset(), AFortWorldManager::ZoneCompletedOffset(), AFortGameModeZone::EndOfZoneRemainTimeOffset(), AFortGameModeZone::ActiveSpawnPadOffset()))
    {
        return;
    }

    const int32_t Index = Natives::AFortMissionManager_OnZoneEndIndex;
    const int32_t StatusChangeIndex = Natives::AFortMissionManager_OnMissionStatusChangeIndex;
    UObject* MissionManagerDefault = StaticClass()->GetDefaultObject(false);
    void* const StatusChange = MissionManagerDefault ? (*reinterpret_cast<void***>(MissionManagerDefault))[StatusChangeIndex] : nullptr;
    OnMissionStatusChangeOG = reinterpret_cast<decltype(OnMissionStatusChangeOG)>(StatusChange);
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && VTable[Index] == Natives::SharedEmptyFunction)
        {
            Hooks::SwapVTable(L"AFortMissionManager::OnZoneEnd", VTable, Index, reinterpret_cast<void*>(&OnZoneEndHK));
        }
        if (VTable && StatusChange && VTable[StatusChangeIndex] == StatusChange)
        {
            Hooks::SwapVTable(L"AFortMissionManager::OnMissionStatusChange", VTable, StatusChangeIndex, reinterpret_cast<void*>(&OnMissionStatusChangeHK));
        }
    }
}

void AFortMissionManager::OnZoneEndHK(AFortMissionManager* This, bool bZoneCompleted)
{
    This->OnZoneEnd(bZoneCompleted);
}

void AFortMissionManager::OnMissionStatusChangeHK(AFortMissionManager* This, AFortMission* Mission, EFortMissionStatus NewStatus)
{
    This->OnMissionStatusChange(Mission, NewStatus);
}
