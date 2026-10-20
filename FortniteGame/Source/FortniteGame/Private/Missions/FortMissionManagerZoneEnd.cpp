#include "pch.h"
#include "Missions/FortMissionManager.h"
#include "Engine/World.h"
#include "Finder.h"
#include "FortGameModeZone.h"
#include "FortGameState.h"
#include "FortWorldManager.h"
#include "Hooks.h"
#include "Log.h"
#include "Missions/FortMission.h"
#include "Natives.h"
#include "Player/FortPlayerController.h"
#include "Quests/FortQuestManager.h"
#include "Templates/Casts.h"
#include "Templates/SharedPointer.h"
#include "UObject/Class.h"
#include "UObject/CoreOnline.h"
#include "UObject/UObjectIterator.h"

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
        const FString MissionName = Info ? Info->GetName() : FString(L"None");
        const FString StatusName = GetMissionStatusName(Status);
        for (AFortPlayerController* PlayerController : Mission->GetParticipatingMissionControllers())
        {
            UFortQuestManager* QuestManager = PlayerController ? PlayerController->GetQuestManager() : nullptr;
            if (!QuestManager)
            {
                continue;
            }

            if (Status == EFortMissionStatus::Succeeded && Info)
            {
                FGameplayTagContainer SourceTags;
                FGameplayTagContainer ContextTags;
                QuestManager->GetSourceAndContextTags(SourceTags, ContextTags);
                const FGameplayTagContainer& MissionTags = Info->MissionTags;
                QuestManager->SendStatEvent(EFortQuestObjectiveStatEvent::Complete, Mission, MissionTags, SourceTags, ContextTags, 1, true);
            }

            if (!SessionId.IsEmpty())
            {
                QuestManager->SendEndPrimaryMission(SessionId, MissionName, StatusName, 0, PlayerController->bShouldReceiveCriticalMatchBonus);
            }
        }
    }

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
