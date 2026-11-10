#pragma once

#include "Containers/Array.h"
#include "FortTheaterInfo.h"
#include "FortZoneTheme.h"
#include "GameplayTagContainer.h"
#include "StructArray.h"
#include "UObject/AssetPtr.h"
#include "UObject/Object.h"

class UFortMissionInfo;
class UFortQuestItemDefinition;

struct FFortGlobalMission
{
    DefineUnrealStruct(FFortGlobalMission, FortniteGame)
    DefineUProperty(TAssetPtr<UFortMissionInfo>, MissionInfo)
    DefineUProperty(TArray<EFortTheaterType>, AllowedTheaterTypes)
    DefineUProperty(FGameplayTagQuery, TheaterTagQuery)
    DefineUProperty(FGameplayTagQuery, RegionTagQuery)
    DefineUProperty(TArray<EFortZoneType>, AllowedZoneTypes)
    DefineUProperty(FGameplayTagQuery, ZoneTagQuery)
    DefineUProperty(FGameplayTagQuery, PrimaryMissionTagQuery)
    DefineUProperty(float, MaxDifficultyLevel)
    DefineUProperty(float, MinDifficultyLevel)
    DefineBitfieldUProperty(bIsPrototype)
    DefineBitfieldUProperty(bAllowInTestMaps)
};

struct FFortQuestDrivenMission : public FFortGlobalMission
{
    DefineUnrealStruct(FFortQuestDrivenMission, FortniteGame)
    DefineUProperty(UFortQuestItemDefinition*, RequiredQuest)
};

// 5.10 (CL-4240749) groups the quest-driven missions into named sub-lists that can each be switched off.
struct FQuestDrivenMissionSubList
{
    DefineUnrealStruct(FQuestDrivenMissionSubList, FortniteGame)
    DefineUProperty(bool, bEnabled)
    DefineUStructArrayProperty(FFortQuestDrivenMission, QuestDrivenMissionList)
};

class UFortSharedMissionLists : public UObject
{
    DefineUnrealClass(UFortSharedMissionLists, FortniteGame)

public:
    DefineUStructArrayProperty(FFortGlobalMission, SharedMissionList)
    // Up to 5.0.
    DefineUStructArrayProperty(FFortQuestDrivenMission, QuestDrivenMissionList)
    // 5.10 onwards, in place of QuestDrivenMissionList.
    DefineUStructArrayProperty(FQuestDrivenMissionSubList, QuestDrivenMissionLists)

    // Every quest-driven mission: the one list up to 5.0, the enabled sub-lists' from 5.10.
    TArray<FFortQuestDrivenMission*> GetQuestDrivenMissions() const
    {
        TArray<FFortQuestDrivenMission*> Missions;
        if (Member_QuestDrivenMissionList.IsAvailable())
        {
            TStructArray<FFortQuestDrivenMission>& List = QuestDrivenMissionList;
            for (int32_t Index = 0; Index < List.Num(); ++Index)
            {
                Missions.Add(&List[Index]);
            }
        }
        else if (Member_QuestDrivenMissionLists.IsAvailable())
        {
            TStructArray<FQuestDrivenMissionSubList>& SubLists = QuestDrivenMissionLists;
            for (int32_t SubListIndex = 0; SubListIndex < SubLists.Num(); ++SubListIndex)
            {
                FQuestDrivenMissionSubList& SubList = SubLists[SubListIndex];
                TStructArray<FFortQuestDrivenMission>& List = SubList.QuestDrivenMissionList;
                for (int32_t Index = 0; SubList.bEnabled && Index < List.Num(); ++Index)
                {
                    Missions.Add(&List[Index]);
                }
            }
        }
        return Missions;
    }
};
