#include "pch.h"
#include "FortVisibilityManager.h"
#include "Engine/World.h"
#include "FortGameState.h"
#include "FortVisibilityComponent.h"
#include "Templates/Casts.h"

TArray<AActor*>& AFortVisibilityManager::GetObservers() const
{
    FVisibiltyInfoArray& InfoArray = VisibilityInfoArray;
    return *reinterpret_cast<TArray<AActor*>*>(reinterpret_cast<uintptr_t>(&InfoArray) - sizeof(TArray<AActor*>));
}

void AFortVisibilityManager::RegisterVisibilityComponent(UFortVisibilityComponent* VisibilityComponent)
{
#if WITH_SERVER_CODE
    AActor* const ComponentOwner = VisibilityComponent ? VisibilityComponent->GetOwner() : nullptr;
    if (!ComponentOwner)
    {
        return;
    }

    FVisibiltyInfoArray& InfoArray = VisibilityInfoArray;
    TStructArray<FVisibilityInfo>& Infos = InfoArray.VisibilityInfoArray;
    for (int32_t Index = 0; Index < Infos.Num(); ++Index)
    {
        if (Infos[Index].VisibilityComponent == VisibilityComponent)
        {
            return;
        }
    }

    FVisibilityInfo& Info = Infos.AddDefaulted();
    Info.Actor = ComponentOwner;
    Info.VisibilityComponent = VisibilityComponent;
    Info.TeamVisibilityFlag = 0;
    InfoArray.MarkItemDirty(Info);
#endif
}

void AFortVisibilityManager::UnregisterVisibilityComponent(UFortVisibilityComponent* VisibilityComponent)
{
#if WITH_SERVER_CODE
    FVisibiltyInfoArray& InfoArray = VisibilityInfoArray;
    TStructArray<FVisibilityInfo>& Infos = InfoArray.VisibilityInfoArray;
    bool bRemoved = false;
    for (int32_t Index = Infos.Num() - 1; Index >= 0; --Index)
    {
        if (Infos[Index].VisibilityComponent == VisibilityComponent)
        {
            Infos.RemoveAt(Index);
            bRemoved = true;
        }
    }

    if (bRemoved)
    {
        InfoArray.MarkArrayDirty();
    }
#endif
}

void AFortVisibilityManager::RegisterObserver(AActor* Observer)
{
#if WITH_SERVER_CODE
    TArray<AActor*>& Observers = GetObservers();
    if (Observer && !Observers.Contains(Observer))
    {
        Observers.Add(Observer);
    }
#endif
}

void AFortVisibilityManager::UnregisterObserver(AActor* Observer)
{
#if WITH_SERVER_CODE
    TArray<AActor*>& Observers = GetObservers();
    for (int32_t Index = Observers.Num() - 1; Index >= 0; --Index)
    {
        if (Observers[Index] == Observer)
        {
            Observers.RemoveAt(Index);
        }
    }
#endif
}

AFortVisibilityManager* AFortVisibilityManager::Get(const AActor* WorldContextActor)
{
    UWorld* World = WorldContextActor ? WorldContextActor->GetWorld() : nullptr;
    AFortGameState* GameState = World ? Cast<AFortGameState>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    return GameState ? static_cast<AFortVisibilityManager*>(GameState->VisibilityManager) : nullptr;
}
