#include "pch.h"
#include "AI/FortQueryGenerators.h"
#include "Engine/World.h"
#include "Finder.h"
#include "Hooks.h"
#include "Log.h"
#include "Natives.h"
#include "Pawns/FortPawn.h"
#include "Player/FortPlayerController.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"
#include "UObject/WeakObjectPtr.h"

#include <cfloat>
#include <cstring>
#include <new>

namespace
{
    constexpr size_t GoalItemSize = 0x20;
    constexpr size_t GoalInfoSize = 0x18;

    AFortAIController* QueryController(void* QueryInstance, APawn*& OutPawn)
    {
        OutPawn = nullptr;
        if (Natives::FEnvQueryInstance_Owner < 0)
        {
            return nullptr;
        }
        const FWeakObjectPtr& OwnerPtr = *reinterpret_cast<const FWeakObjectPtr*>(static_cast<uint8_t*>(QueryInstance) + Natives::FEnvQueryInstance_Owner);
        OutPawn = Cast<APawn>(OwnerPtr.Get());
        return OutPawn ? Cast<AFortAIController>(reinterpret_cast<UObject*>(static_cast<AController*>(OutPawn->Controller))) : nullptr;
    }

    void AddGoalItem(void* QueryInstance, AActor* Actor, UObject* Assignment, const uint8_t* GoalInfo)
    {
        uint8_t* Instance = static_cast<uint8_t*>(QueryInstance);
        if (*reinterpret_cast<const uint16_t*>(Instance + Natives::FEnvQueryInstance_ValueSize) < GoalItemSize)
        {
            return;
        }

        const uint64_t Item = Natives::FEnvQueryInstance_AddItemData(QueryInstance, FWeakObjectPtr(reinterpret_cast<UObject*>(Actor)));
        const uint32_t DataOffset = static_cast<uint32_t>(Item >> 32) & 0x7FFFFFFF;
        uint8_t* Data = *reinterpret_cast<uint8_t**>(Instance + Natives::FEnvQueryInstance_RawData) + DataOffset;
        if (GoalInfo)
        {
            std::memcpy(Data, GoalInfo, GoalInfoSize);
        }
        else
        {
            const FVector InvalidLocation(FLT_MAX, FLT_MAX, FLT_MAX);
            std::memcpy(Data + 8, &InvalidLocation, sizeof(FVector));
        }
        if (Assignment)
        {
            new (Data + GoalInfoSize) FWeakObjectPtr(Assignment);
        }
    }

    bool HasValidGoal(const uint8_t* GoalInfo)
    {
        if (reinterpret_cast<const FWeakObjectPtr*>(GoalInfo)->Get())
        {
            return true;
        }
        const FVector& Location = *reinterpret_cast<const FVector*>(GoalInfo + 8);
        return Location.X != FLT_MAX || Location.Y != FLT_MAX || Location.Z != FLT_MAX;
    }

    TArray<AActor*> PerceivedHostileActors(AFortAIController* Controller)
    {
        UObject* Perception = Controller->PerceptionComponent;
        UFunction* Function = Perception ? Perception->FindFunction(FName(L"GetPerceivedHostileActors", FNAME_Find)) : nullptr;
        if (!Function)
        {
            return {};
        }
        Reflection::FParams Params(Function, true);
        return Params.Call(Perception) ? Params.Take<TArray<AActor*>>(L"OutActors") : TArray<AActor*>();
    }
}

void UFortQueryGenerator_Enemies::GenerateItems(void* QueryInstance) const
{
#if WITH_SERVER_CODE
    APawn* Pawn = nullptr;
    AFortAIController* Controller = QueryController(QueryInstance, Pawn);
    if (!Controller || !AFortAIGoalManager::GetCurrent(Controller))
    {
        return;
    }

    TArray<AActor*> Enemies;
    if (bPerceivedEnemiesOnly)
    {
        Enemies = PerceivedHostileActors(Controller);
    }
    else
    {
        UWorld* World = Controller->GetWorld();
        for (TObjectIterator<AFortPlayerController> It; It; ++It)
        {
            if (It->GetWorld() == World && It->MyFortPawn)
            {
                Enemies.Add(reinterpret_cast<AActor*>(static_cast<AFortPawn*>(It->MyFortPawn)));
            }
        }
    }

    for (AActor* Enemy : Enemies)
    {
        AFortPawn* EnemyPawn = Cast<AFortPawn>(reinterpret_cast<UObject*>(Enemy));
        if (Enemy && !(EnemyPawn && EnemyPawn->IsDead()))
        {
            AddGoalItem(QueryInstance, Enemy, nullptr, nullptr);
        }
    }
#endif
}

void UFortQueryGenerator_SpecificAssignmentGoals::GenerateItems(void* QueryInstance) const
{
#if WITH_SERVER_CODE
    APawn* Pawn = nullptr;
    AFortAIController* Controller = QueryController(QueryInstance, Pawn);
    AFortAIGoalManager* GoalManager = Controller ? AFortAIGoalManager::GetCurrent(Controller) : nullptr;
    if (!GoalManager || Natives::UFortAIAssignment_Goals < 0)
    {
        return;
    }

    const FFortAIAssignmentIdentifier& Identifier = AssignmentIdentifier;
    const EAssignmentType Type = Identifier.AssignmentType;
    UObject* Encounter = Type == EAssignmentType::GetEncounter() ? Controller->GetEncounterInfo() : nullptr;
    UObject* Assignment = GoalManager->FindAssignment(Identifier, Encounter);
    if (!Assignment)
    {
        return;
    }

    // As the game's generator (1.9 0x30B8C0) does: only an assignment this pawn can use, and only the goals this AI can
    // take. The goal loop rejects the others anyway, and a query option with only rejected goals keeps the query from
    // falling through to its next option, such as AllEnemiesAndDefaultWorld's enemies after a world assignment of
    // placement actors (#143: a mimic spawned with no perceived enemy got no target and turned back into a chest).
    const bool bChecksGoals = Natives::UFortAIAssignment_CanPawnUse && Natives::FFortAIGoal_CanAIUse;
    if (bChecksGoals && !Natives::UFortAIAssignment_CanPawnUse(Assignment, Pawn))
    {
        return;
    }
    UObject* Settings = static_cast<UFortAIAssignment*>(Assignment)->GetAssignmentSettings();

    const FScriptArray& Goals = *reinterpret_cast<const FScriptArray*>(reinterpret_cast<uint8_t*>(Assignment) + Natives::UFortAIAssignment_Goals);
    const int32_t GoalSize = FFortAIGoal::StaticStruct()->GetStructureSize();
    for (int32_t Index = 0; Index < Goals.Num(); ++Index)
    {
        const uint8_t* Goal = static_cast<const uint8_t*>(Goals.GetData()) + static_cast<size_t>(Index) * GoalSize;
        if (HasValidGoal(Goal) && (!bChecksGoals || Natives::FFortAIGoal_CanAIUse(Goal, Controller, Settings)))
        {
            AddGoalItem(QueryInstance, reinterpret_cast<AActor*>(reinterpret_cast<const FWeakObjectPtr*>(Goal)->Get()), Assignment, Goal);
        }
    }
#endif
}

void UFortQueryGenerator_Enemies::HookVTables()
{
    UObject* EnemiesDefault = StaticClass()->GetDefaultObject(false);
    UObject* AssignmentGoalsDefault = UFortQueryGenerator_SpecificAssignmentGoals::StaticClass()->GetDefaultObject(false);
    void** EnemiesVTable = EnemiesDefault ? *reinterpret_cast<void***>(EnemiesDefault) : nullptr;
    void** AssignmentGoalsVTable = AssignmentGoalsDefault ? *reinterpret_cast<void***>(AssignmentGoalsDefault) : nullptr;
    if (!EnemiesVTable || !AssignmentGoalsVTable || !Finder::ResolveQueryGeneratorsLive(EnemiesVTable))
    {
        return;
    }

    Finder::ResolveAssignmentGoalChecksLive(UFortAIAssignmentSettings::MaxAIAllowedForAssignmentOffset(), UFortAIAssignmentSettings::MaxAIAllowedPerGoalOffset());

    const int32_t Index = Natives::UEnvQueryGenerator_GenerateItemsIndex;
    Hooks::SwapVTable(L"UFortQueryGenerator_Enemies::GenerateItems", EnemiesVTable, Index, reinterpret_cast<void*>(&GenerateItemsHK));
    Hooks::SwapVTable(L"UFortQueryGenerator_SpecificAssignmentGoals::GenerateItems", AssignmentGoalsVTable, Index, reinterpret_cast<void*>(&UFortQueryGenerator_SpecificAssignmentGoals::GenerateItemsHK));
}

void UFortQueryGenerator_Enemies::GenerateItemsHK(UFortQueryGenerator_Enemies* This, void* QueryInstance)
{
    This->GenerateItems(QueryInstance);
}

void UFortQueryGenerator_SpecificAssignmentGoals::GenerateItemsHK(UFortQueryGenerator_SpecificAssignmentGoals* This, void* QueryInstance)
{
    This->GenerateItems(QueryInstance);
}
