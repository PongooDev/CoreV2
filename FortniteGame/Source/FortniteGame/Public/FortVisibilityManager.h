#pragma once

#include "Containers/Array.h"
#include "Engine/NetSerialization.h"
#include "GameFramework/Actor.h"
#include "StructArray.h"

class UFortVisibilityComponent;

struct FVisibilityInfo : public FFastArraySerializerItem
{
    DefineUnrealStruct(FVisibilityInfo, FortniteGame)
    DefineUProperty(AActor*, Actor)
    DefineUProperty(UFortVisibilityComponent*, VisibilityComponent)
    DefineUProperty(uint16_t, TeamVisibilityFlag)
};

struct FVisibiltyInfoArray : public FFastArraySerializer
{
    DefineUnrealStruct(FVisibiltyInfoArray, FortniteGame)
    DefineUStructArrayProperty(FVisibilityInfo, VisibilityInfoArray)
};

class AFortVisibilityManager : public AActor
{
    DefineUnrealClass(AFortVisibilityManager, FortniteGame)

public:
    DefineUStructProperty(FVisibiltyInfoArray, VisibilityInfoArray)

    TArray<AActor*>& GetObservers() const;

    void RegisterVisibilityComponent(UFortVisibilityComponent* VisibilityComponent);
    void UnregisterVisibilityComponent(UFortVisibilityComponent* VisibilityComponent);
    void RegisterObserver(AActor* Observer);
    void UnregisterObserver(AActor* Observer);

    static AFortVisibilityManager* Get(const AActor* WorldContextActor);
};
