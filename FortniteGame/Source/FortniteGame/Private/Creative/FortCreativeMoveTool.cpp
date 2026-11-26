#include "pch.h"
#include "Creative/FortCreativeMoveTool.h"
#include "Athena/FortGameStateAthena.h"
#include "Building/BuildingSMActor.h"
#include "Components/PrimitiveComponent.h"
#include "Creative/FortVolume.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "Finder.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "GameplayTagContainer.h"
#include "HAL/IConsoleManager.h"
#include "HAL/UnrealMemory.h"
#include "Hooks.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Log.h"
#include "Math/Rotator.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "Player/FortPlayerController.h"
#include "PlaysetLevelStreamComponent.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include "UObject/WeakObjectPtr.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <xmmintrin.h>

namespace
{
    // ECollisionChannel::ECC_WorldStatic; ECollisionResponse ECR_Overlap and ECR_Block.
    constexpr uint8_t ECC_WorldStatic = 0;
    constexpr uint8_t ECR_Overlap = 1;
    constexpr uint8_t ECR_Block = 2;
    // EFortStructuralGridQueryResults: CanAdd, ExistingActor, ... ForcedAdd (4); 5 is refused with the rest.
    constexpr int32_t GridCanAdd = 0;
    constexpr int32_t GridExistingActor = 1;
    constexpr int32_t GridForcedAdd = 4;
    // ESelectionProperty: one actor, several, several with a grid piece, none.
    constexpr uint8_t SelectionSingle = 0;
    constexpr uint8_t SelectionMultiple = 1;
    constexpr uint8_t SelectionMultipleOnGrid = 2;
    constexpr uint8_t SelectionNone = 3;

    UClass* ClassByPath(const wchar_t* Path)
    {
        return FindObject<UClass>(nullptr, Path);
    }

    bool IsChildOf(const UObject* Object, const UClass* Class)
    {
        return Object && Class && Object->GetClass()->IsChildOf(Class);
    }

    bool IsLive(const UObject* Object)
    {
        return Object && !Object->IsPendingKill();
    }

    int32_t ConsoleInt(const wchar_t* Name, int32_t Default)
    {
        IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
        return Variable ? Variable->GetInt() : Default;
    }

    float ConsoleFloat(const wchar_t* Name, float Default)
    {
        IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
        return Variable ? Variable->GetFloat() : Default;
    }

    // A TArray parameter of a reflected call, filled with Count elements copied from Source (plain data only); the
    // parameters' own destruction frees it.
    bool SetArrayParameter(Reflection::FParams& Params, const wchar_t* Name, const void* Source, int32_t Count, int32_t ElementSize)
    {
        FScriptArray* Array = static_cast<FScriptArray*>(Params.GetAddress(Name));
        if (!Array)
        {
            return false;
        }
        if (Count > 0)
        {
            Array->Add(Count, ElementSize);
            std::memcpy(Array->GetData(), Source, static_cast<size_t>(Count) * ElementSize);
        }
        return true;
    }

    // Epic's FMath::RoundToInt (the SSE cvt, round to nearest, of 2F + 0.5, halved).
    int32_t RoundToInt(float F)
    {
        return _mm_cvt_ss2si(_mm_set_ss(F + F + 0.5f)) >> 1;
    }

    float SignOf(float Value)
    {
        return Value > 0.0f ? 1.0f : (Value < 0.0f ? -1.0f : 0.0f);
    }

    // 8.51 0x1850950: the building grid point nearest Location (512 across, 384 up).
    FVector SnapToBuildingGrid(const FVector& Location)
    {
        const auto Across = [](float Value)
        {
            int32_t Cells = RoundToInt(std::fabs(Value)) + 256;
            Cells -= Cells % 512;
            return static_cast<float>(static_cast<int32_t>(static_cast<float>(Cells) * SignOf(Value)));
        };
        const int32_t Up = 384 * ((RoundToInt(std::fabs(Location.Z)) + 192) / 384);
        return FVector(Across(Location.X), Across(Location.Y), static_cast<float>(static_cast<int32_t>(static_cast<float>(Up) * SignOf(Location.Z))));
    }

    // 8.51 0x18465A0: a scale axis kept within CreativeMoveTool.MovingFreely_ScaleMin/Max, on the side of Previous.
    float ClampScale(float Previous, float Scale)
    {
        const float Min = ConsoleFloat(L"CreativeMoveTool.MovingFreely_ScaleMin", 0.333f);
        const float Max = ConsoleFloat(L"CreativeMoveTool.MovingFreely_ScaleMax", 3.0f);
        if (Previous > Min)
        {
            if (Scale >= Min)
            {
                return std::fmin(Max, Scale);
            }
            return Scale < -Max ? -Max : std::fmin(-Min, Scale);
        }
        if (Scale > -Min)
        {
            return Scale < Min ? Min : std::fmin(Max, Scale);
        }
        return Scale < -Max ? -Max : std::fmin(-Min, Scale);
    }

    FBox ComponentsBoundingBox(AActor* Actor, bool bNonColliding)
    {
        FBox Box;
        CallVirtual<FBox*>(Actor, Natives::AActor_GetComponentsBoundingBoxIndex, &Box, bNonColliding);
        return Box;
    }

    FBox ComponentsBoundingBoxInLocalSpace(AActor* Actor, bool bNonColliding)
    {
        FBox Box;
        CallVirtual<FBox*>(Actor, Natives::AActor_CalculateComponentsBoundingBoxInLocalSpaceIndex, &Box, bNonColliding);
        return Box;
    }

    // ICreativeMoveToolOverrides::Execute_GetCollisionStaticMesh.
    UStaticMesh* GetCollisionStaticMesh(AActor* Actor)
    {
        UFunction* Function = Actor->FindFunction(FName(L"GetCollisionStaticMesh"));
        Reflection::FParams Params(Function, true);
        return Function && Params.Call(Actor) ? Params.Take<UStaticMesh*>(L"ReturnValue") : nullptr;
    }

    // The box the phone moves an actor by, in the actor's own space (8.51 CalculateBounds and SpawnActorWithTransform):
    // an override's collision mesh, a building's static mesh, else its components. Null when neither of the first two.
    bool MeshBoundsOf(AActor* Actor, FBox& OutBox)
    {
        static UClass* OverridesInterface = ClassByPath(L"/Script/FortniteGame.CreativeMoveToolOverrides");
        if (OverridesInterface && UKismetSystemLibrary::DoesImplementInterface(Actor, OverridesInterface))
        {
            UStaticMesh* Mesh = GetCollisionStaticMesh(Actor);
            if (!IsLive(Mesh))
            {
                return false;
            }
            OutBox = Mesh->GetBoundingBox();
            return true;
        }
        if (IsChildOf(Actor, ABuildingSMActor::StaticClass()))
        {
            UStaticMeshComponent* Component = static_cast<ABuildingSMActor*>(Actor)->StaticMeshComponent;
            if (Component && IsLive(static_cast<UStaticMesh*>(Component->StaticMesh)))
            {
                FVector Min;
                FVector Max;
                Component->GetLocalBounds(Min, Max);
                OutBox = FBox(Min, Max);
                return true;
            }
        }
        return false;
    }

    struct FTraceHit
    {
        bool bHit = false;
        FVector ImpactPoint;
        AActor* Actor = nullptr;
    };

    // UWorld::LineTraceSingleByChannel / LineTraceMultiByChannel on ECC_WorldStatic, every channel's response Response,
    // ignoring IgnoredActors. The query parameters are UKismetSystemLibrary's (its ConfigureCollisionParams), where 8.51
    // built its own: they differ only in returning the physical material and face index, which nothing here reads.
    std::vector<FTraceHit> LineTrace(UWorld* World, const FVector& Start, const FVector& End, uint8_t Response, const TArray<AActor*>& IgnoredActors, bool bMulti)
    {
        std::vector<FTraceHit> Hits;
        static UScriptStruct* HitStruct = FindObject<UScriptStruct>(nullptr, L"/Script/Engine.HitResult");
        const UProperty* ImpactPointProperty = HitStruct ? HitStruct->FindPropertyByName(FName(L"ImpactPoint", FNAME_Find)) : nullptr;
        const UProperty* ActorProperty = HitStruct ? HitStruct->FindPropertyByName(FName(L"Actor", FNAME_Find)) : nullptr;
        if (!World || !ImpactPointProperty || !ActorProperty)
        {
            return Hits;
        }
        alignas(16) uint8_t QueryParams[0x100] = {};
        using FConfigure = void* (*)(void*, FName, bool, const TArray<AActor*>*, bool, UObject*);
        reinterpret_cast<FConfigure>(Natives::KismetSystemLibrary_ConfigureCollisionParams)(QueryParams, FName(), false, &IgnoredActors, false, nullptr);
        uint8_t ResponseParams[32];
        std::memset(ResponseParams, Response, sizeof(ResponseParams));

        const int32_t HitSize = HitStruct->GetStructureSize();
        const auto Read = [&](const uint8_t* Hit)
        {
            FTraceHit Result;
            Result.bHit = true;
            Result.ImpactPoint = *reinterpret_cast<const FVector*>(Hit + ImpactPointProperty->GetOffset_ForInternal());
            FWeakObjectPtr Weak;
            std::memcpy(&Weak, Hit + ActorProperty->GetOffset_ForInternal(), sizeof(Weak));
            Result.Actor = Cast<AActor>(Weak.Get());
            return Result;
        };
        if (bMulti)
        {
            FScriptArray OutHits;
            using FMultiTrace = bool (*)(UWorld*, FScriptArray*, const FVector*, const FVector*, uint8_t, const void*, const void*);
            reinterpret_cast<FMultiTrace>(Natives::UWorld_LineTraceMultiByChannel)(World, &OutHits, &Start, &End, ECC_WorldStatic, QueryParams, ResponseParams);
            for (int32_t Index = 0; Index < OutHits.Num(); ++Index)
            {
                Hits.push_back(Read(static_cast<const uint8_t*>(OutHits.GetData()) + static_cast<size_t>(Index) * HitSize));
            }
            OutHits.Empty();
        }
        else
        {
            std::vector<uint8_t> OutHit(HitSize, 0);
            // FHitResult's constructor leaves Time at 1 (and the weak pointers unset).
            *reinterpret_cast<float*>(OutHit.data() + 8) = 1.0f;
            const FWeakObjectPtr None;
            std::memcpy(OutHit.data() + ActorProperty->GetOffset_ForInternal(), &None, sizeof(None));
            using FSingleTrace = bool (*)(UWorld*, void*, const FVector*, const FVector*, uint8_t, const void*, const void*);
            reinterpret_cast<FSingleTrace>(Natives::UWorld_LineTraceSingleByChannel)(World, OutHit.data(), &Start, &End, ECC_WorldStatic, QueryParams, ResponseParams);
            Hits.push_back(Read(OutHit.data()));
            Hits.back().bHit = Hits.back().Actor != nullptr;
        }
        // ~FCollisionQueryParams: its ignored components' and actors' heap allocations.
        for (const size_t Allocation : { 0x40, 0x60 })
        {
            if (void* Memory = *reinterpret_cast<void**>(QueryParams + Allocation))
            {
                FMemory::Free(Memory);
            }
        }
        return Hits;
    }

    void SwapStubInSubclasses(UFunction* Function, void* Replacement, const wchar_t* Name)
    {
        UClass* ToolClass = AFortCreativeMoveTool::StaticClass();
        const int32_t Index = Function ? Finder::ImplementationIndex(Function) : -1;
        UObject* Default = ToolClass && Index >= 0 ? ToolClass->GetDefaultObject(false) : nullptr;
        void** DefaultVTable = Default ? *reinterpret_cast<void***>(Default) : nullptr;
        if (!DefaultVTable)
        {
            Log::Error(L"Finder: AFortCreativeMoveTool::{}_Implementation slot not found", Name);
            return;
        }
        void* const Stub = DefaultVTable[Index];
        if (!Finder::IsStubFunction(reinterpret_cast<uintptr_t>(Stub)))
        {
            Log::Info(L"Finder: AFortCreativeMoveTool::{}_Implementation (slot 0x{:X}) is native in this build", Name, Index);
            return;
        }
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Class = *It;
            UObject* ClassDefault = Class->IsChildOf(ToolClass) ? Class->GetDefaultObject(false) : nullptr;
            void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
            if (VTable && VTable[Index] == Stub)
            {
                Hooks::SwapVTable(std::format(L"AFortCreativeMoveTool::{}_Implementation", Name), VTable, Index, Replacement);
            }
        }
    }

    // #685: the restored code's refusals, for reports of copies that fail.
    std::wstring NameOf(const UObject* Object)
    {
        return Object ? Object->GetName().ToStdString() : std::wstring(L"none");
    }

    std::wstring ClassNameOf(const UObject* Object)
    {
        return Object ? Object->GetClass()->GetName().ToStdString() : std::wstring(L"none");
    }

    uintptr_t ImplementationOf(UFunction* Function)
    {
        UObject* Default = Function ? AFortCreativeMoveTool::StaticClass()->GetDefaultObject(false) : nullptr;
        const int32_t Index = Default ? Finder::ImplementationIndex(Function) : -1;
        return Index >= 0 ? reinterpret_cast<uintptr_t>((*reinterpret_cast<void***>(Default))[Index]) : 0;
    }
}

void UObjectInteractionBehavior::StartCreativeInteractionOnServer()
{
    // A BlueprintNativeEvent: the UHT thunk calls the object's own version (FindFunctionChecked by name), as 8.51's
    // StartInteracting did (0x19B8D10). 9.00's CreativeInteraction_MoveObjectsFreely overrides it to spawn the selection's
    // previews (B_CreativeMoveTool_Athena's ServerSpawnPreview), what shows the copied props blue (#681).
    UFunction* Base = FindStaticFunction_StartCreativeInteractionOnServer();
    UFunction* Own = Base ? FindFunction(Base->GetFName()) : nullptr;
    Reflection::FParams Params(Own ? Own : Base, true);
    Params.Call(this);
}

void UObjectInteractionBehavior::PlayGameplayAbility(UClass* ActivatedAbilityClass)
{
    Reflection::FParams Params(FindStaticFunction_PlayGameplayAbility(), true);
    if (Params.Set(L"ActivatedAbilityClass", ActivatedAbilityClass))
    {
        Params.Call(this);
    }
}

UClass* FActorTemplateRecord::GetActorClass() const
{
    FWeakObjectPtr Weak;
    static_assert(sizeof(Weak) == sizeof(ActorClassWeak));
    std::memcpy(&Weak, ActorClassWeak, sizeof(Weak));
    return Cast<UClass>(Weak.Get());
}

bool& AFortCreativeMoveTool::bClientNeedsToWaitForReplication() const
{
    return *reinterpret_cast<bool*>(reinterpret_cast<uintptr_t>(this) + Member_SelectedActors.GetOffsetChecked() - 0x10);
}

int32_t& AFortCreativeMoveTool::NumberOfReplicatedSelectedActors() const
{
    return *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(this) + Member_SelectedActors.GetOffsetChecked() - 0xC);
}

bool& AFortCreativeMoveTool::bIsFirstDragTick() const
{
    return *reinterpret_cast<bool*>(reinterpret_cast<uintptr_t>(this) + Member_SelectionSpaceActorsBounds.GetOffsetChecked() + sizeof(FBox));
}

FTransform& AFortCreativeMoveTool::CameraToWorldAtDragStart() const
{
    // After SelectionToWorldAtDragStart (bounds + 0x20).
    return *reinterpret_cast<FTransform*>(reinterpret_cast<uintptr_t>(this) + Member_SelectionSpaceActorsBounds.GetOffsetChecked() + 0x50);
}

float& AFortCreativeMoveTool::TargetTractorBeamOffset() const
{
    return *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(this) + Member_bIsAutoTractorBeamRunning.GetOffsetChecked() + 4);
}

float& AFortCreativeMoveTool::TractorBeamOffset() const
{
    return *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(this) + Member_bIsAutoTractorBeamRunning.GetOffsetChecked() + 8);
}

uint8_t* AFortCreativeMoveTool::CachedMoveToolSettings() const
{
    // FMoveToolSettings: the gravity and scaling options a selection of one or no actors had.
    return reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(this) + Member_SelectionProperty.GetOffsetChecked() + 1);
}

AFortPlayerController* AFortCreativeMoveTool::GetOwningFortPlayerController() const
{
    APawn* Pawn = Cast<APawn>(static_cast<UObject*>(GetOwner()));
    return Pawn ? Cast<AFortPlayerController>(static_cast<UObject*>(Pawn->Controller)) : nullptr;
}

UObjectInteractionBehavior* AFortCreativeMoveTool::DetermineActorMoveMode(const TArray<AActor*>& Actors)
{
    using FDetermine = UObjectInteractionBehavior* (*)(AFortCreativeMoveTool*, const TArray<AActor*>*);
    return reinterpret_cast<FDetermine>(Natives::AFortCreativeMoveTool_DetermineActorMoveMode)(this, &Actors);
}

UObjectInteractionBehavior* AFortCreativeMoveTool::GetInteractionBehaviorForActor(AActor* Actor)
{
    using FGet = UObjectInteractionBehavior* (*)(AFortCreativeMoveTool*, AActor*);
    return reinterpret_cast<FGet>(Natives::AFortCreativeMoveTool_GetInteractionBehaviorForActor)(this, Actor);
}

bool AFortCreativeMoveTool::IsOverlappingIdenticalActor(AActor* Actor, const FTransform& Transform)
{
    using FIsOverlapping = bool (*)(AFortCreativeMoveTool*, AActor*, const FTransform*);
    return reinterpret_cast<FIsOverlapping>(Natives::AFortCreativeMoveTool_IsOverlappingIdenticalActor)(this, Actor, &Transform);
}

uint8_t AFortCreativeMoveTool::GetSelectionPropertyForActors()
{
    // 8.51 0x184D900.
    TStructArray<FCreativeSelectedActorInfo>& Selected = SelectedActors;
    if (Selected.Num() <= 1)
    {
        return Selected.Num() == 1 ? SelectionSingle : SelectionNone;
    }
    for (int32_t Index = 0; Index < Selected.Num(); ++Index)
    {
        AActor* Actor = Selected[Index].Actor;
        UObjectInteractionBehavior* Behavior = Actor ? GetInteractionBehaviorForActor(Actor) : nullptr;
        if (IsChildOf(Behavior, UMoveBuildingsOnGrid::StaticClass()))
        {
            return SelectionMultipleOnGrid;
        }
    }
    return SelectionMultiple;
}

void AFortCreativeMoveTool::CalculateBounds(FTransform& OutSelectionToWorld, FBox& OutSelectionSpaceActorsBounds)
{
    // 8.51 0x1847760. The selection's frame is the first actor's location and unsigned scale with no rotation; the box,
    // every actor's box in that frame, is then moved so the frame sits at its bottom centre.
    TStructArray<FCreativeSelectedActorInfo>& Selected = SelectedActors;
    OutSelectionToWorld = FTransform();
    for (int32_t Index = 0; Index < Selected.Num(); ++Index)
    {
        if (AActor* First = Selected[Index].Actor)
        {
            const FTransform FirstTransform = First->GetTransform();
            OutSelectionToWorld.Translation = FirstTransform.Translation;
            OutSelectionToWorld.Scale3D = FVector(std::fabs(FirstTransform.Scale3D.X), std::fabs(FirstTransform.Scale3D.Y), std::fabs(FirstTransform.Scale3D.Z));
            break;
        }
    }
    OutSelectionToWorld.Rotation = FRotator(0.0f, 0.0f, 0.0f).Quaternion();

    FBox Bounds;
    for (int32_t Index = 0; Index < Selected.Num(); ++Index)
    {
        AActor* Actor = Selected[Index].Actor;
        if (!Actor)
        {
            continue;
        }
        const FTransform ActorToSelection = Actor->GetTransform() * OutSelectionToWorld.Inverse();
        FBox LocalBox;
        if (!MeshBoundsOf(Actor, LocalBox))
        {
            LocalBox = ComponentsBoundingBoxInLocalSpace(Actor, false);
        }
        Bounds += LocalBox.TransformBy(ActorToSelection);
    }

    const FVector Pivot((Bounds.Min.X + Bounds.Max.X) * 0.5f, (Bounds.Min.Y + Bounds.Max.Y) * 0.5f, Bounds.Min.Z);
    OutSelectionToWorld.Translation = OutSelectionToWorld.TransformPosition(Pivot);
    OutSelectionSpaceActorsBounds = FBox(Bounds.Min - Pivot, Bounds.Max - Pivot);
}

void AFortCreativeMoveTool::StartInteracting(const TArray<AActor*>& Actors)
{
    // 8.51 0x1873610.
    if (UObjectInteractionBehavior* Active = ActiveMovementMode)
    {
        const TStructArray<FCreativeSelectedActorInfo>& Held = SelectedActors;
        Log::Info(L"Creative phone: {} is still in {} with {} selected actor(s), so {} new actor(s) aren't selected", NameOf(this), NameOf(Active), Held.Num(), Actors.Num());
        return;
    }
    ActiveMovementMode = DetermineActorMoveMode(Actors);
    UObjectInteractionBehavior* MovementMode = ActiveMovementMode;
    if (!MovementMode)
    {
        return;
    }
    TStructArray<FCreativeSelectedActorInfo>& Selected = SelectedActors;
    for (AActor* Actor : Actors)
    {
        if (IsLive(Actor))
        {
            Selected.AddDefaulted().Actor = Actor;
        }
    }
    FTransform& SelectionTransform = SelectionToWorld;
    FBox& SelectionBounds = SelectionSpaceActorsBounds;
    CalculateBounds(SelectionTransform, SelectionBounds);

    static UClass* BuildingActorClass = ABuildingActor::StaticClass();
    TMulticastDelegate<AActor*>* RelevancyChanged = static_cast<TMulticastDelegate<AActor*>*>(Natives::BuildingActorRelevancyChanged);
    for (int32_t Index = 0; Index < Selected.Num(); ++Index)
    {
        FCreativeSelectedActorInfo& Info = Selected[Index];
        AActor* Actor = Info.Actor;
        Info.ActorToSelectionAtDragStart = Actor->GetTransform() * SelectionTransform.Inverse();
        Info.DragStartGridSnapPoint = SelectionTransform.Translation - SnapToBuildingGrid(SelectionTransform.Translation);

        FVector Scale = Actor->GetActorScale3D();
        if (IsChildOf(Actor, BuildingActorClass))
        {
            const float Baseline = 1.0f / std::fabs(static_cast<ABuildingActor*>(Actor)->BaselineScale);
            Scale = Scale * Baseline;
        }
        FVector& Modifier = ScaleModifier;
        Modifier = FVector(ClampScale(Modifier.X, Scale.X), ClampScale(Modifier.Y, Scale.Y), ClampScale(Modifier.Z, Scale.Z));

        Info.bWasCollisionEnabled = Actor->bActorEnableCollision;
        if (Info.bWasCollisionEnabled)
        {
            Actor->SetActorEnableCollision(false);
            if (!bClientNeedsToWaitForReplication())
            {
                Reflection::FParams Params(FindStaticFunction_MulticastSetCollisionForActor(), true);
                Params.Set(L"Actor", Actor);
                Params.Set(L"bEnableCollision", false);
                Params.Call(this);
            }
        }

        Info.OriginalRelevancyDistance = 0.0f;
        if (IsChildOf(Actor, BuildingActorClass))
        {
            Info.OriginalRelevancyDistance = static_cast<ABuildingActor*>(Actor)->CullDistance;
            RelevancyChanged->Broadcast(Actor);
        }
        else
        {
            Info.OriginalRelevancyDistance = std::sqrt(static_cast<float>(Actor->NetCullDistanceSquared));
        }
        Info.bWasDormant = Actor->NetDormancy > DORM_Awake;
        if (Info.bWasDormant)
        {
            Actor->SetNetDormancy(DORM_Awake);
        }
        if (IsChildOf(Actor, ABuildingSMActor::StaticClass()))
        {
            reinterpret_cast<void (*)(AActor*)>(Natives::ABuildingSMActor_PrepareForMove)(Actor);
        }
    }

    if (!bClientNeedsToWaitForReplication())
    {
        const int32_t ElementSize = FCreativeSelectedActorInfo::StaticStruct()->GetStructureSize();
        Reflection::FParams Client(FindStaticFunction_ClientStartInteracting(), true);
        Client.Set(L"NewActiveMovementMode", MovementMode);
        SetArrayParameter(Client, L"NewSelectedActors", Selected.GetData(), Selected.Num(), ElementSize);
        Client.Set(L"NewSelectionToWorld", SelectionTransform);
        Client.Set(L"NewSelectionSpaceActorBounds", SelectionBounds);
        Client.Call(this);

        Reflection::FParams Remote(FindStaticFunction_RemoteClientsUpdateSelectedActors(), true);
        SetArrayParameter(Remote, L"NewSelectedActors", Selected.GetData(), Selected.Num(), ElementSize);
        Remote.Set(L"CurrentObjectInteractionBehvaior", MovementMode);
        Remote.Call(this);
    }

    bIsPushPressed = false;
    bIsPullPressed = false;
    bIsAutoTractorBeamRunning = ConsoleInt(L"CreativeMoveTool.MovingFreely_EnableAutoTractorBeam", 1) != 0;
    TargetTractorBeamOffset() = 0.0f;
    TractorBeamOffset() = 0.0f;
    TargetRotationOffset = FQuat();
    RotationOffset = FQuat();
    bIsRotateClockwisePressed = false;
    bIsRotateCounterclockwisePressed = false;
    bIsFirstDragTick() = true;
    bIsPreviewingMove = true;
    MovementMode->StartCreativeInteractionOnServer();

    const uint8_t NewSelectionProperty = GetSelectionPropertyForActors();
    const uint8_t OldSelectionProperty = SelectionProperty;
    if (NewSelectionProperty != OldSelectionProperty)
    {
        if (OldSelectionProperty == SelectionSingle)
        {
            CachedMoveToolSettings()[0] = bAllowGravityOnPlace;
            CachedMoveToolSettings()[1] = bIsScalingInsteadOfRotating;
        }
        SelectionProperty = NewSelectionProperty;
        uint8_t Parameters = NewSelectionProperty;
        const FMulticastScriptDelegate& Changed = OnSelectionPropertyChanged;
        Changed.ProcessMulticastDelegate(&Parameters);
    }
}

AActor* AFortCreativeMoveTool::SpawnActorWithTransform(AActor* ActorToSpawn, FTransform TargetTransform, bool bAllowGravity, bool bAllowOverlap, bool bIgnoreStructuralIssues,
    bool bForPreviewing)
{
    // 8.51 0x1871CB0.
    AActor* Spawned = nullptr;
    const auto Finish = [this, &Spawned, ActorToSpawn]()
    {
        if (Spawned)
        {
            if (IsChildOf(Spawned, ABuildingActor::StaticClass()))
            {
                CallVirtual<void>(Spawned, Natives::ABuildingActor_ResolveCollisionTouchesIndex);
            }
            TArray<FOriginalAndSpawnedPair>& Placed = NewlyPlacedActors;
            Placed.Add(FOriginalAndSpawnedPair{ ActorToSpawn, Spawned });
            bClientNeedsToProcessNewlyPlacedActors = true;
        }
        if (!Spawned && GetNetMode() == NM_DedicatedServer)
        {
            if (UObjectInteractionBehavior* MovementMode = ActiveMovementMode)
            {
                UClass* FailAbility = MovementMode->FailAbility;
                static UClass* GameplayAbilityClass = ClassByPath(L"/Script/FortniteGame.FortGameplayAbility");
                MovementMode->PlayGameplayAbility(FailAbility && GameplayAbilityClass && FailAbility->IsChildOf(GameplayAbilityClass) ? FailAbility : nullptr);
            }
        }
        return Spawned;
    };
    if (!ActorToSpawn)
    {
        return Finish();
    }

    if (!IsChildOf(ActorToSpawn, ABuildingActor::StaticClass()))
    {
        if (UWorld* World = GetWorld())
        {
            Spawned = World->SpawnActor(ActorToSpawn->GetClass(), &TargetTransform, FActorSpawnParameters());
        }
        return Finish();
    }

    AFortPlayerController* Controller = GetOwningFortPlayerController();
    if (!Controller)
    {
        Log::Info(L"Creative phone: {} can't copy {}: the phone has no player controller", NameOf(this), ClassNameOf(ActorToSpawn));
        return Finish();
    }

    FBox Bounds = ComponentsBoundingBox(ActorToSpawn, true);
    FBox LocalBox;
    if (MeshBoundsOf(ActorToSpawn, LocalBox))
    {
        Bounds = LocalBox.TransformBy(ActorToSpawn->GetTransform());
    }

    if (bAllowGravity && ConsoleInt(L"CreativeMoveTool.MovingFreely_UseGravity", 1) != 0)
    {
        UWorld* World = GetWorld();
        const FVector Centre((Bounds.Max.X + Bounds.Min.X) * 0.5f, (Bounds.Max.Y + Bounds.Min.Y) * 0.5f, 0.0f);
        const FVector Start(Centre.X, Centre.Y, ConsoleFloat(L"CreativeMoveTool.Drops_HitcastOffset", 10.0f) + Bounds.Min.Z);
        TArray<AActor*> Ignored;
        Ignored.Add(ActorToSpawn);
        const std::vector<FTraceHit> Floor = LineTrace(World, Start, FVector(Centre.X, Centre.Y, -10000.0f), ECR_Block, Ignored, false);
        if (!Floor.empty() && Floor.front().bHit)
        {
            bool bDrop = true;
            AActor* FloorActor = Floor.front().Actor;
            static UClass* StaticReplicatedClass = ClassByPath(L"/Script/FortniteGame.FortStaticReplicatedActor");
            if (IsChildOf(FloorActor, AStaticMeshActor::StaticClass()) || IsChildOf(FloorActor, StaticReplicatedClass))
            {
                // A static floor over the landscape: dropped only when no landscape is between it and the actor's top.
                static UClass* LandscapeProxyClass = ClassByPath(L"/Script/Landscape.LandscapeStreamingProxy");
                static UClass* LandscapeClass = ClassByPath(L"/Script/Landscape.Landscape");
                Ignored.Add(FloorActor);
                for (const FTraceHit& Hit : LineTrace(World, Floor.front().ImpactPoint, FVector(Centre.X, Centre.Y, Bounds.Max.Z), ECR_Overlap, Ignored, true))
                {
                    if (IsLive(Hit.Actor) && (IsChildOf(Hit.Actor, LandscapeProxyClass) || IsChildOf(Hit.Actor, LandscapeClass)))
                    {
                        bDrop = false;
                    }
                }
            }
            if (bDrop)
            {
                TargetTransform.Translation.Z = Floor.front().ImpactPoint.Z - (Bounds.Min.Z - TargetTransform.Translation.Z);
            }
        }
    }

    FVector Location = TargetTransform.Translation;
    FRotator Rotation = TargetTransform.Rotation.Rotator();
    TArray<ABuildingActor*> ExistingBuildings;
    uint8_t MarkerOptionalAdjustment = 0;
    const int32_t QueryResult = CallVirtual<int32_t>(Controller, Natives::AFortPlayerController_CanPlaceBuildableClassInStructuralGridIndex, ActorToSpawn->GetClass(), &Location,
        &Rotation, false, &ExistingBuildings, &MarkerOptionalAdjustment);

    AFortGameStateAthena* GameState = Cast<AFortGameStateAthena>(static_cast<UObject*>(GetWorld() ? GetWorld()->GetGameState() : nullptr));
    AFortVolumeManager* VolumeManager = GameState ? static_cast<AFortVolumeManager*>(GameState->VolumeManager) : nullptr;
    VolumeManager = IsLive(VolumeManager) ? VolumeManager : nullptr;
    ULevel* Level = nullptr;
    if (VolumeManager)
    {
        const FVector Top(TargetTransform.Translation.X, TargetTransform.Translation.Y, TargetTransform.Translation.Z + std::fabs(Bounds.Max.Z - Bounds.Min.Z));
        AFortVolume* Volume = VolumeManager->GetVolumeForLocation(Top);
        if (!Volume)
        {
            Log::Info(L"Creative phone: {} can't copy {}: no island volume at ({:.0f}, {:.0f}, {:.0f})", NameOf(this), ClassNameOf(ActorToSpawn), Top.X, Top.Y, Top.Z);
            return nullptr;
        }
        TArray<AActor*> ActorsToRemove;
        for (ABuildingActor* Existing : ExistingBuildings)
        {
            ActorsToRemove.Add(IsChildOf(Existing, AActor::StaticClass()) ? static_cast<AActor*>(Existing) : nullptr);
        }
        const FFortCreativeBudgetTracker& Budget = Volume->BudgetTracker;
        const bool bCanAfford = Budget.CanAddActor(ActorToSpawn, ActorsToRemove);
        UPlaysetLevelStreamComponent* LevelStream = UPlaysetLevelStreamComponent::GetForActor(reinterpret_cast<AActor*>(Volume));
        ULevelStreaming* Streamed = LevelStream ? static_cast<ULevelStreaming*>(static_cast<UObject*>(LevelStream->StreamedLevel)) : nullptr;
        Level = Streamed ? static_cast<ULevel*>(Streamed->LoadedLevel) : nullptr;
        if (!bCanAfford)
        {
            Log::Info(L"Creative phone: {} can't copy {}: the island's budget refused it ({} existing building(s) to replace, preview {})", NameOf(this), ClassNameOf(ActorToSpawn),
                ActorsToRemove.Num(), bForPreviewing);
            return Finish();
        }
    }

    const bool bGridAllows = QueryResult == GridCanAdd || QueryResult == GridExistingActor || QueryResult == GridForcedAdd;
    const bool bStructureAllows = bIgnoreStructuralIssues || bGridAllows;
    const bool bOverlapAllows = bStructureAllows && (bAllowOverlap || !IsOverlappingIdenticalActor(ActorToSpawn, TargetTransform));
    if (!bOverlapAllows)
    {
        Log::Info(L"Creative phone: {} can't copy {}: {} (structural grid result {})", NameOf(this), ClassNameOf(ActorToSpawn),
            bStructureAllows ? L"an identical actor overlaps" : L"the structural grid refused it", QueryResult);
    }
    if (bOverlapAllows)
    {
        if (bShouldDestroyPropsWhenPlacing)
        {
            const FGameplayTag StructuralDeath{ FName(L"Gameplay.Effect.InstantDeath.StructuralSupport") };
            for (ABuildingActor* Existing : ExistingBuildings)
            {
                if (Existing && Existing->bDestroyOnPlayerBuildingPlacement)
                {
                    Existing->Die(StructuralDeath, nullptr, nullptr);
                }
            }
        }

        FActorTemplateRecord Template;
        reinterpret_cast<void (*)(AActor*, FActorTemplateRecord*, int64_t)>(Natives::LevelSave_CreateTemplate)(ActorToSpawn, &Template, 0);
        UWorld* World = ActorToSpawn->GetWorld();
        UClass* TemplateClass = Template.GetActorClass();
        if (World && TemplateClass)
        {
            FActorSpawnParameters SpawnParameters;
            SpawnParameters.Instigator = Controller->GetPawn();
            SpawnParameters.OverrideLevel = Level;
            SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            SpawnParameters.bNoFail = true;
            SpawnParameters.bDeferConstruction = true;
            Spawned = World->SpawnActor(TemplateClass, &TargetTransform, SpawnParameters);
        }
        ABuildingSMActor* SpawnedBuilding = Cast<ABuildingSMActor>(static_cast<UObject*>(Spawned));
        if (bForPreviewing && SpawnedBuilding)
        {
            SpawnedBuilding->bRegisterWithStructuralGrid = false;
            SpawnedBuilding->bIsForPreviewing = true;
        }
        if (Spawned)
        {
            reinterpret_cast<void (*)(AActor*, const FTransform*, const FActorTemplateRecord*, void*)>(Natives::LevelSave_ApplyTemplate)(Spawned, &TargetTransform, &Template, nullptr);
        }
        if (SpawnedBuilding)
        {
            reinterpret_cast<void (*)(AActor*)>(Natives::ABuildingSMActor_ApplySavedSupportStatus)(SpawnedBuilding);
            // Set opposite first: SetMirrored does nothing when the flag already matches.
            const bool bMirrored = TargetTransform.Scale3D.X < 0.0f;
            SpawnedBuilding->bMirrored = !bMirrored;
            CallVirtual<void>(SpawnedBuilding, Natives::ABuildingSMActor_SetMirroredIndex, bMirrored);
        }
        if (VolumeManager)
        {
            VolumeManager->ResaveActor(Spawned);
        }
        static UClass* LevelDuplicatable = ClassByPath(L"/Script/FortniteGame.LevelDuplicatable");
        if (Spawned && LevelDuplicatable && UKismetSystemLibrary::DoesImplementInterface(Spawned, LevelDuplicatable))
        {
            UFunction* PostFinishDuplication = Spawned->FindFunction(FName(L"PostFinishDuplication"));
            Reflection::FParams Params(PostFinishDuplication, true);
            if (PostFinishDuplication && Params.Set(L"SourceObject", static_cast<UObject*>(ActorToSpawn)))
            {
                Params.Call(Spawned);
            }
        }
        if (QueryResult == GridExistingActor && !bForPreviewing)
        {
            for (ABuildingActor* Existing : ExistingBuildings)
            {
                if (static_cast<AActor*>(Existing) != ActorToSpawn && Existing)
                {
                    Existing->Destroy(true, true);
                }
            }
        }
    }
    return Finish();
}

void AFortCreativeMoveTool::ServerStartInteracting_Implementation(const TArray<AActor*>& Actors, const FTransform& DragStart)
{
    // 8.51 0x186ECA0.
    FFlashCountedActorInfo& Cut = CutActors;
    Cut.Actors.Empty();
    Cut.FlashCounter = static_cast<int8_t>(Cut.FlashCounter + 1);
    UObjectInteractionBehavior* MoveMode = DetermineActorMoveMode(Actors);
    if (!MoveMode)
    {
        return;
    }
    if (!IsChildOf(MoveMode, UMoveObjectsFreely::StaticClass()))
    {
        CameraToWorldAtDragStart() = DragStart;
        StartInteracting(Actors);
        return;
    }

    // Props are cut: copies are spawned to move, and the originals hidden until the move ends.
    Reflection::FParams Duplicate(FindStaticFunction_ServerDuplicateStartInteracting(), true);
    SetArrayParameter(Duplicate, L"Actors", Actors.GetData(), Actors.Num(), sizeof(AActor*));
    Duplicate.Set(L"DragStart", DragStart);
    Duplicate.Call(this);
    if (!IsChildOf(static_cast<UObjectInteractionBehavior*>(ActiveMovementMode), UMoveObjectsFreely::StaticClass()))
    {
        return;
    }
    for (AActor* Actor : Actors)
    {
        // Core's: a null from the client is skipped (8.51 reads through it).
        if (!Actor)
        {
            continue;
        }
        FCreativeSelectedActorInfo& Info = Cut.Actors.AddDefaulted();
        Info.Actor = Actor;
        Info.bWasCollisionEnabled = Actor->bActorEnableCollision;
        Cut.FlashCounter = static_cast<int8_t>(Cut.FlashCounter + 1);
        Actor->SetActorEnableCollision(false);
        Actor->SetActorHiddenInGame(true);
    }
    bClientNeedsToProcessNewlyPlacedActors = false;
}

void AFortCreativeMoveTool::ServerDuplicateStartInteracting_Implementation(const TArray<AActor*>& Actors, const FTransform& DragStart)
{
    // 8.51 0x186E0E0. Grid pieces are copied far below the map, where they meet no other piece, until they are placed.
    FFlashCountedActorInfo& Cut = CutActors;
    Cut.Actors.Empty();
    Cut.FlashCounter = static_cast<int8_t>(Cut.FlashCounter + 1);
    if (Actors.Num() <= 0)
    {
        return;
    }
    TArray<FOriginalAndSpawnedPair>& Placed = NewlyPlacedActors;
    if (bClientNeedsToWaitForReplication() || Placed.Num() > 0)
    {
        const TStructArray<FCreativeSelectedActorInfo>& Held = SelectedActors;
        Log::Info(L"Creative phone: {} copies {} actor(s) while the last copy waits: {} of {} selected actor(s) reported by the client (waiting {}), {} placed pair(s) unreported",
            NameOf(this), Actors.Num(), NumberOfReplicatedSelectedActors(), Held.Num(), bClientNeedsToWaitForReplication(), Placed.Num());
    }
    Placed.Reset();
    UObjectInteractionBehavior* MoveMode = DetermineActorMoveMode(Actors);
    if (!MoveMode)
    {
        Log::Info(L"Creative phone: {} can't copy {} actor(s) (the first {}): no interaction behaviour takes them", NameOf(this), Actors.Num(), ClassNameOf(Actors[0]));
        return;
    }
    const bool bOnGrid = IsChildOf(MoveMode, UMoveBuildingsOnGrid::StaticClass());
    TArray<AActor*> Copies;
    for (AActor* Actor : Actors)
    {
        if (!Actor)
        {
            Copies.Add(nullptr);
            continue;
        }
        FTransform Transform = Actor->GetTransform();
        if (bOnGrid)
        {
            Transform.Translation.Z = ConsoleFloat(L"CreativeMoveTool.MovingOnGrid_LowSpawnHeight", -5500.0f);
        }
        Copies.Add(SpawnActorWithTransform(Actor, Transform, false, true, true, true));
    }
    NumberOfReplicatedSelectedActors() = 0;
    bClientNeedsToWaitForReplication() = true;
    bClientNeedsToProcessNewlyPlacedActors = true;
    CameraToWorldAtDragStart() = DragStart;
    StartInteracting(Copies);
}

void AFortCreativeMoveTool::ServerSpawnActorWithTransform_Implementation(AActor* ActorToSpawn, const FTransform& TargetTransform, bool bAllowOverlap, bool bAllowGravity,
    bool bIgnoreStructuralIssues, bool bForPreviewing)
{
    // 8.51 0x186EBF0.
    AActor* Spawned = SpawnActorWithTransform(ActorToSpawn, TargetTransform, bAllowGravity, bAllowOverlap, bIgnoreStructuralIssues, bForPreviewing);
    ABuildingSMActor* Building = Cast<ABuildingSMActor>(static_cast<UObject*>(Spawned));
    if (!bForPreviewing && Building && Building->bRegisterWithStructuralGrid)
    {
        reinterpret_cast<void (*)(AActor*)>(Natives::ABuildingSMActor_AddToStructuralGrid)(Building);
    }
}

void AFortCreativeMoveTool::ServerSpawnSelectedActorsWithTransform_Implementation(bool bAllowOverlap, bool bAllowGravity, bool bIgnoreStructuralIssues, bool bForPreviewing)
{
    // Core's (no build has the body): what 8.51's client asked one actor at a time (0x185B5D0: the selection lowest first,
    // ServerSpawnActorWithTransform with each actor's transform), now that 9.00's client asks once with the options alone
    // (0x193A490). The server has the selection, moved by ServerMoveSelectionSet.
    TStructArray<FCreativeSelectedActorInfo>& Selected = SelectedActors;
    TArray<AActor*> Actors;
    for (int32_t Index = 0; Index < Selected.Num(); ++Index)
    {
        if (AActor* Actor = Selected[Index].Actor)
        {
            Actors.Add(Actor);
        }
    }
    std::stable_sort(Actors.begin(), Actors.end(), [](AActor* A, AActor* B) { return A->GetTransform().Translation.Z < B->GetTransform().Translation.Z; });
    const TArray<FOriginalAndSpawnedPair>& Placed = NewlyPlacedActors;
    for (AActor* Actor : Actors)
    {
        const int32_t PlacedBefore = Placed.Num();
        ServerSpawnActorWithTransform_Implementation(Actor, Actor->GetTransform(), bAllowOverlap, bAllowGravity, bIgnoreStructuralIssues, bForPreviewing);
        if (Placed.Num() == PlacedBefore)
        {
            Log::Info(L"Creative phone: {} didn't place {} ({})", NameOf(this), NameOf(Actor), ClassNameOf(Actor));
        }
    }
    const FMulticastScriptDelegate& Finished = OnFinishedSpawningSelectedActors;
    Finished.ProcessMulticastDelegate(nullptr);
}

void AFortCreativeMoveTool::ServerAddToQuickBar_Implementation(const TArray<AActor*>& Actors, int32_t DesiredSlotIndex)
{
    // Core's (no build has the body). 9.00's client (HandleAddToQuickBarPressed, 0x1939300) sends the selection and its
    // creative quickbar slot, then keeps its own ULevelSaveRecord of the actors (CurrentQuickbarRequestSaveRecord) until
    // ClientAddToQuickBarComplete says which slot took it. The prefab item itself is the profile's (MCP), which Core's
    // backend doesn't serve yet: the server answers with the slot it was asked for when the selection is still there.
    bool bSelectionValid = Actors.Num() > 0;
    for (AActor* Actor : Actors)
    {
        bSelectionValid = bSelectionValid && IsLive(Actor);
    }
    Reflection::FParams Params(FindStaticFunction_ClientAddToQuickBarComplete(), true);
    if (Params.Set(L"SlotPutIn", bSelectionValid ? DesiredSlotIndex : -1))
    {
        Params.Call(this);
    }
}

void AFortCreativeMoveTool::ServerStartInteracting_ImplementationHK(AFortCreativeMoveTool* This, const TArray<AActor*>& Actors, const FTransform& DragStart)
{
    This->ServerStartInteracting_Implementation(Actors, DragStart);
}

void AFortCreativeMoveTool::ServerDuplicateStartInteracting_ImplementationHK(AFortCreativeMoveTool* This, const TArray<AActor*>& Actors, const FTransform& DragStart)
{
    This->ServerDuplicateStartInteracting_Implementation(Actors, DragStart);
}

void AFortCreativeMoveTool::ServerSpawnActorWithTransform_ImplementationHK(AFortCreativeMoveTool* This, AActor* ActorToSpawn, const FTransform& TargetTransform, bool bAllowOverlap,
    bool bAllowGravity, bool bIgnoreStructuralIssues, bool bForPreviewing)
{
    This->ServerSpawnActorWithTransform_Implementation(ActorToSpawn, TargetTransform, bAllowOverlap, bAllowGravity, bIgnoreStructuralIssues, bForPreviewing);
}

void AFortCreativeMoveTool::ServerSpawnSelectedActorsWithTransform_ImplementationHK(AFortCreativeMoveTool* This, bool bAllowOverlap, bool bAllowGravity, bool bIgnoreStructuralIssues,
    bool bForPreviewing)
{
    This->ServerSpawnSelectedActorsWithTransform_Implementation(bAllowOverlap, bAllowGravity, bIgnoreStructuralIssues, bForPreviewing);
}

void AFortCreativeMoveTool::ServerAddToQuickBar_ImplementationHK(AFortCreativeMoveTool* This, const TArray<AActor*>& Actors, int32_t DesiredSlotIndex)
{
    This->ServerAddToQuickBar_Implementation(Actors, DesiredSlotIndex);
}

void AFortCreativeMoveTool::RestoreStrippedServerFunctions()
{
#if WITH_SERVER_CODE
    // 9.00 (CL-6337466) adds ServerSpawnSelectedActorsWithTransform and strips the phone's selection and spawning server
    // code; 7.00 to 8.51 have it natively, older builds no phone.
    if (!StaticClass() || !FindStaticFunction_ServerSpawnSelectedActorsWithTransform())
    {
        return;
    }
    UObject* BuildingDefault = ABuildingActor::StaticClass() ? ABuildingActor::StaticClass()->GetDefaultObject(false) : nullptr;
    UClass* KismetSystem = UKismetSystemLibrary::StaticClass();
    UFunction* LineTraceSingle = KismetSystem ? KismetSystem->FindFunctionByName(FName(L"LineTraceSingle")) : nullptr;
    UFunction* LineTraceMulti = KismetSystem ? KismetSystem->FindFunctionByName(FName(L"LineTraceMulti")) : nullptr;
    UFunction* GetActorBounds = AActor::FindStaticFunction_GetActorBounds();
    const ptrdiff_t ClassTracker = FFortCreativeBudgetTracker::ClassTrackerOffset();

    Finder::FCreativeMoveToolAnchors Anchors;
    Anchors.HandleCopyPickupPressed = ImplementationOf(FindStaticFunction_HandleCopyPickupPressed());
    Anchors.ServerClearMovementMode = ImplementationOf(FindStaticFunction_ServerClearMovementMode());
    Anchors.ClientStartInteracting = ImplementationOf(FindStaticFunction_ClientStartInteracting());
    Anchors.BuildingAnimationOffset = ABuildingSMActor::BuildingAnimationOffset();
    Anchors.InteractionBehaviorsOffset = Member_InteractionBehaviors.IsAvailable() ? Member_InteractionBehaviors.GetOffsetChecked() : -1;
    Anchors.BuildingActorVTable = BuildingDefault ? *reinterpret_cast<void***>(BuildingDefault) : nullptr;
    Anchors.BuildingActorVTableSlots = 0x200;
    Anchors.SavedDirectlySupportedStatusOffset = ABuildingSMActor::SavedDirectlySupportedStatusOffset();
    Anchors.ChildClassesAtLimitOffset = ClassTracker >= 0 && FFortCreativeClassInstanceTracker::ChildClassesAtLimitOffset() >= 0
        ? ClassTracker + FFortCreativeClassInstanceTracker::ChildClassesAtLimitOffset() : -1;
    Anchors.ParentClassesAtLimitOffset = ClassTracker >= 0 && FFortCreativeClassInstanceTracker::ParentClassesAtLimitOffset() >= 0
        ? ClassTracker + FFortCreativeClassInstanceTracker::ParentClassesAtLimitOffset() : -1;
    Anchors.GetActorBoundsExec = GetActorBounds ? reinterpret_cast<uintptr_t>(GetActorBounds->GetNativeFunc()) : 0;
    Anchors.LineTraceSingleExec = LineTraceSingle ? reinterpret_cast<uintptr_t>(LineTraceSingle->GetNativeFunc()) : 0;
    Anchors.LineTraceMultiExec = LineTraceMulti ? reinterpret_cast<uintptr_t>(LineTraceMulti->GetNativeFunc()) : 0;
    const bool bResolved = Finder::ResolveCreativeMoveToolLive(Anchors);
    const bool bBuilding = Natives::AFortPlayerController_CanPlaceBuildableClassInStructuralGridIndex >= 0 && Natives::ABuildingActor_DieIndex >= 0
        && Natives::ABuildingSMActor_SetMirroredIndex >= 0;
    if (!bResolved || !bBuilding || FCreativeSelectedActorInfo::StaticStruct()->GetStructureSize() != 0x60)
    {
        Log::Error(L"Finder: AFortCreativeMoveTool's server functions aren't restored: a piece they need wasn't found (finder {}, building helpers {})",
            bResolved ? L"complete" : L"incomplete", bBuilding ? L"found" : L"missing");
        return;
    }

    SwapStubInSubclasses(FindStaticFunction_ServerStartInteracting(), reinterpret_cast<void*>(&ServerStartInteracting_ImplementationHK), L"ServerStartInteracting");
    SwapStubInSubclasses(FindStaticFunction_ServerDuplicateStartInteracting(), reinterpret_cast<void*>(&ServerDuplicateStartInteracting_ImplementationHK), L"ServerDuplicateStartInteracting");
    SwapStubInSubclasses(FindStaticFunction_ServerSpawnActorWithTransform(), reinterpret_cast<void*>(&ServerSpawnActorWithTransform_ImplementationHK), L"ServerSpawnActorWithTransform");
    SwapStubInSubclasses(FindStaticFunction_ServerSpawnSelectedActorsWithTransform(), reinterpret_cast<void*>(&ServerSpawnSelectedActorsWithTransform_ImplementationHK),
        L"ServerSpawnSelectedActorsWithTransform");
    SwapStubInSubclasses(FindStaticFunction_ServerAddToQuickBar(), reinterpret_cast<void*>(&ServerAddToQuickBar_ImplementationHK), L"ServerAddToQuickBar");
#endif
}
