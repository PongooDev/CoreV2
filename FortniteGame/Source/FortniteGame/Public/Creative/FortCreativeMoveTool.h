#pragma once

#include "Components/ActorComponent.h"
#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Math/Box.h"
#include "Math/Transform.h"
#include "Reflection.h"
#include "StructArray.h"
#include "UObject/ScriptDelegates.h"
#include "Weapons/FortWeapon.h"

class AActor;
class AFortPlayerController;
class AFortVolume;
class AFortVolumeManager;
class UFortCreativeUserPrefabItemDefinition;

// The phone's ways of moving a selection (Creative, from 7.00).
class UObjectInteractionBehavior : public UActorComponent
{
    DefineUnrealClass(UObjectInteractionBehavior, FortniteGame)
    DefineUFunction(StartCreativeInteractionOnServer)
    DefineUFunction(PlayGameplayAbility)

public:
    DefineUProperty(UClass*, FailAbility)
    DefineUProperty(int32_t, Priority)

    void StartCreativeInteractionOnServer();
    void PlayGameplayAbility(UClass* ActivatedAbilityClass);
};

class UMoveObjectsFreely : public UObjectInteractionBehavior
{
    DefineUnrealClass(UMoveObjectsFreely, FortniteGame)
};

class UMoveBuildingsOnGrid : public UObjectInteractionBehavior
{
    DefineUnrealClass(UMoveBuildingsOnGrid, FortniteGame)
};

struct FCreativeSelectedActorInfo
{
    DefineUnrealStruct(FCreativeSelectedActorInfo, FortniteGame)

public:
    DefineUProperty(AActor*, Actor)
    DefineUProperty(FTransform, ActorToSelectionAtDragStart)
    DefineUProperty(FVector, DragStartGridSnapPoint)
    DefineUProperty(float, OriginalRelevancyDistance)
    DefineUProperty(bool, bWasCollisionEnabled)
    DefineUProperty(bool, bWasDormant)
};

struct FOriginalAndSpawnedPair
{
    AActor* OriginalActor = nullptr;
    AActor* SpawnedActor = nullptr;
};

static_assert(sizeof(FOriginalAndSpawnedPair) == 0x10);

struct FFlashCountedActorInfo
{
    DefineUnrealStruct(FFlashCountedActorInfo, FortniteGame)

public:
    DefineUStructArrayProperty(FCreativeSelectedActorInfo, Actors)
    DefineUProperty(int8_t, FlashCounter)
};

// The level save's copy of an actor (Epic's FActorTemplateRecord, unreflected): its class, its components' and its own
// SaveGame properties, as ULevelSaveRecord's CreateTemplate writes it (9.00 0x152B510, 8.51 0x1484DE0).
struct FActorTemplateRecord
{
    struct FComponentRecord
    {
        FName ComponentName;
        int32_t ComponentClassWeak[2];
        int32_t ComponentClassTag;
        int32_t Padding;
        FName ComponentClassPath;
        FString ComponentClassSubPath;
        TArray<uint8_t> ComponentData;
    };

    FActorTemplateRecord() = default;
    FActorTemplateRecord(const FActorTemplateRecord&) = delete;
    FActorTemplateRecord& operator=(const FActorTemplateRecord&) = delete;
    ~FActorTemplateRecord()
    {
        for (FComponentRecord& Component : ActorComponents)
        {
            Component.ComponentClassSubPath.~FString();
            Component.ComponentData.~TArray();
        }
    }

    // The class the record spawns (its TSoftClassPtr, set from a live class by CreateTemplate).
    UClass* GetActorClass() const;

    int64_t ID = 0;
    int32_t ActorClassWeak[2] = { -1, 0 };
    int32_t ActorClassTag = 0;
    int32_t Padding = 0;
    FName ActorClassPath;
    FString ActorClassSubPath;
    TArray<FComponentRecord> ActorComponents;
    TArray<uint8_t> ActorData;
};

static_assert(sizeof(FActorTemplateRecord::FComponentRecord) == 0x40);
static_assert(sizeof(FActorTemplateRecord) == 0x50);

// The Creative phone (Epic's AFortCreativeMoveTool, Creative/FortCreativeMoveTool.cpp). 9.00 (CL-6337466) compiled its
// selection and spawning server code out; Core restores it from 8.51's (CL-6165369).
class AFortCreativeMoveTool : public AFortWeapon
{
    DefineUnrealClass(AFortCreativeMoveTool, FortniteGame)
    DefineUFunction(ServerStartInteracting)
    DefineUFunction(ServerDuplicateStartInteracting)
    DefineUFunction(ServerSpawnActorWithTransform)
    DefineUFunction(ServerSpawnSelectedActorsWithTransform)
    DefineUFunction(ServerAddToQuickBar)
    DefineUFunction(ServerClearMovementMode)
    DefineUFunction(ClientStartInteracting)
    DefineUFunction(ClientAddToQuickBarComplete)
    DefineUFunction(ServerClearQuickBarSlot)
    DefineUFunction(ClientClearQuickBarSlotComplete)
    DefineUFunction(RemoteClientsUpdateSelectedActors)
    DefineUFunction(MulticastSetCollisionForActor)
    DefineUFunction(HandleCopyPickupPressed)

public:
    DefineUProperty(UObjectInteractionBehavior*, ActiveMovementMode)
    DefineUProperty(bool, bIsPreviewingMove)
    DefineUProperty(TArray<UObjectInteractionBehavior*>, InteractionBehaviors)
    DefineUStructArrayProperty(FCreativeSelectedActorInfo, SelectedActors)
    DefineUProperty(FVector, ScaleModifier)
    DefineUProperty(FTransform, SelectionToWorld)
    DefineUProperty(FBox, SelectionSpaceActorsBounds)
    DefineUProperty(uint8_t, SelectionProperty)
    DefineUProperty(bool, bIsQuickbarEnabled)
    DefineUProperty(bool, bIsPushPressed)
    DefineUProperty(bool, bIsPullPressed)
    DefineUProperty(bool, bIsAutoTractorBeamRunning)
    DefineUProperty(bool, bIsRotateClockwisePressed)
    DefineUProperty(bool, bIsRotateCounterclockwisePressed)
    DefineUProperty(bool, bIsScalingInsteadOfRotating)
    DefineUProperty(FQuat, TargetRotationOffset)
    DefineUProperty(FQuat, RotationOffset)
    DefineUProperty(TArray<FOriginalAndSpawnedPair>, NewlyPlacedActors)
    DefineUStructProperty(FFlashCountedActorInfo, CutActors)
    DefineUProperty(bool, bShouldDestroyPropsWhenPlacing)
    DefineUProperty(bool, bAllowGravityOnPlace)
    DefineUProperty(bool, bClientNeedsToProcessNewlyPlacedActors)
    DefineUProperty(FMulticastScriptDelegate, OnSelectionPropertyChanged)
    DefineUProperty(FMulticastScriptDelegate, OnFinishedSpawningSelectedActors)
    DefineUProperty(TArray<UFortCreativeUserPrefabItemDefinition*>, QuickbarSlotItemDefinitions)

    // The RPCs' server halves (the slots 9.00 left as the shared empty function).
    void ServerStartInteracting_Implementation(const TArray<AActor*>& Actors, const FTransform& DragStart);
    void ServerDuplicateStartInteracting_Implementation(const TArray<AActor*>& Actors, const FTransform& DragStart);
    void ServerSpawnActorWithTransform_Implementation(AActor* ActorToSpawn, const FTransform& TargetTransform, bool bAllowOverlap, bool bAllowGravity,
        bool bIgnoreStructuralIssues, bool bForPreviewing);
    void ServerSpawnSelectedActorsWithTransform_Implementation(bool bAllowOverlap, bool bAllowGravity, bool bIgnoreStructuralIssues, bool bForPreviewing);
    void ServerAddToQuickBar_Implementation(const TArray<AActor*>& Actors, int32_t DesiredSlotIndex);
    // 9.10+.
    void ServerClearQuickBarSlot_Implementation(int32_t DesiredSlotIndex);

    // 9.00: swaps the stubs for the restored bodies. Nothing happens on builds without ServerSpawnSelectedActorsWithTransform.
    static void RestoreStrippedServerFunctions();

private:
    void StartInteracting(const TArray<AActor*>& Actors);
    void CalculateBounds(FTransform& OutSelectionToWorld, FBox& OutSelectionSpaceActorsBounds);
    AActor* SpawnActorWithTransform(AActor* ActorToSpawn, FTransform TargetTransform, bool bAllowGravity, bool bAllowOverlap, bool bIgnoreStructuralIssues,
        bool bForPreviewing);
    UObjectInteractionBehavior* DetermineActorMoveMode(const TArray<AActor*>& Actors);
    UObjectInteractionBehavior* GetInteractionBehaviorForActor(AActor* Actor);
    uint8_t GetSelectionPropertyForActors();
    bool IsOverlappingIdenticalActor(AActor* Actor, const FTransform& Transform);
    AFortPlayerController* GetOwningFortPlayerController() const;

    // Unreflected, next to reflected members (9.00 offsets, the same distance from them on 8.51).
    bool& bClientNeedsToWaitForReplication() const;
    int32_t& NumberOfReplicatedSelectedActors() const;
    bool& bIsFirstDragTick() const;
    FTransform& CameraToWorldAtDragStart() const;
    float& TargetTractorBeamOffset() const;
    float& TractorBeamOffset() const;
    uint8_t* CachedMoveToolSettings() const;

    static void ServerStartInteracting_ImplementationHK(AFortCreativeMoveTool* This, const TArray<AActor*>& Actors, const FTransform& DragStart);
    static void ServerDuplicateStartInteracting_ImplementationHK(AFortCreativeMoveTool* This, const TArray<AActor*>& Actors, const FTransform& DragStart);
    static void ServerSpawnActorWithTransform_ImplementationHK(AFortCreativeMoveTool* This, AActor* ActorToSpawn, const FTransform& TargetTransform, bool bAllowOverlap,
        bool bAllowGravity, bool bIgnoreStructuralIssues, bool bForPreviewing);
    static void ServerSpawnSelectedActorsWithTransform_ImplementationHK(AFortCreativeMoveTool* This, bool bAllowOverlap, bool bAllowGravity, bool bIgnoreStructuralIssues,
        bool bForPreviewing);
    static void ServerAddToQuickBar_ImplementationHK(AFortCreativeMoveTool* This, const TArray<AActor*>& Actors, int32_t DesiredSlotIndex);
    static void ServerClearQuickBarSlot_ImplementationHK(AFortCreativeMoveTool* This, int32_t DesiredSlotIndex);
};
