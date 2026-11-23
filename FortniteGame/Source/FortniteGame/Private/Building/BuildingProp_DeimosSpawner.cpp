#include "pch.h"
#include "Building/BuildingProp_DeimosSpawner.h"
#include "AI/FortAIEncounterInfo.h"
#include "Building/BuildingRift.h"
#include "Components/SceneComponent.h"
#include "Components/SphereComponent.h"
#include "Finder.h"
#include "Hooks.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Log.h"
#include "Logging/LogMacros.h"
#include "Natives.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogFortBuilding, Log, All);

namespace
{
    // The selected options, in the order they load: creature count, creature type, spawn timing, spawn limit.
    constexpr int32_t NumSettingsOptions = 4;

    UFunction* HandleAsyncLoadCompleteFunction = nullptr;
    int32_t NextSettingsLoadId = 1;
}

// 8.20 (CL-5625478) strips the device's server half; the rest of it shows what it did:
// - The Blueprint's MinigameStarted (bound to MinigameProgress' start, 0xFC03B0) runs SetupConfigurables, which copies the
//   knobs into the Selected* options and the other members, then calls this native (slot 0xE18 of 0x404A948, the shared
//   empty function; 8.00's slot 0xEB0 too).
// - SpawnRift's exec (0x17673D0) calls the empty function, and so do the callers of the Blueprint's OnSpawnedRift.
// - The settings load is gone but for its log string, "ABuildingProp_DeimosSpawner::HandleAsyncLoadComplete : Failed to
//   load assets for making encounter settings, this will have no overrides specified", and its handle (+0xC00).
// - The rift has bHasLoadedSettings, which its overlaps wait for (0xC52400), so it is there before its settings are.
// - MinigameEnded (0xFBA450, intact) destroys CachedDeimosSpawner; the rift's EndPlay sends RiftDestroyed, and the
//   mission stops its encounter.
void ABuildingProp_DeimosSpawner::MinigameStarted_Implementation()
{
#if WITH_SERVER_CODE
    // Core's: one rift at a time, as MinigameEnded destroys only the one it has.
    if (Role != ROLE_Authority || CachedDeimosSpawner)
    {
        return;
    }
    USceneComponent* Location = SpawnLocation;
    SpawnRift(Location ? Location->GetComponentLocation() : GetTransform().GetLocation());
#endif
}

void ABuildingProp_DeimosSpawner::MinigameStartedHK(ABuildingProp_DeimosSpawner* This)
{
    This->MinigameStarted_Implementation();
}

void ABuildingProp_DeimosSpawner::SpawnRift(const FVector& SpawnPosition)
{
#if WITH_SERVER_CODE
    UClass* RiftClass = DeimosSpawnerClass;
    if (Role != ROLE_Authority || !RiftClass || !RiftClass->IsChildOf(AAthenaCreativeRift::StaticClass()))
    {
        return;
    }
    FTransform SpawnTransform = GetTransform();
    SpawnTransform.Translation = SpawnPosition;
    AAthenaCreativeRift* Rift = Cast<AAthenaCreativeRift>(UGameplayStatics::BeginDeferredActorSpawnFromClass(this, RiftClass, SpawnTransform, ESpawnActorCollisionHandlingMethod::AlwaysSpawn));
    if (!Rift)
    {
        UE_LOG(LogFortBuilding, Error, TEXT("SpawnRift: %s couldn't spawn its %s"), *GetName(), *RiftClass->GetName());
        return;
    }
    Rift->ParentTrap = this;
    // Core's (8.20): the spawn radius knob (1024 to 3072, "2 Tiles" to "6 Tiles") is the radius the rift searches its
    // spawn slots in, and the invincible knob keeps the rift from taking damage. Set before BeginPlay, which uses them.
    if (Member_OverrideQueryRadius.IsAvailable() && OverrideQueryRadius > 0.0f)
    {
        Rift->QueryRadius = OverrideQueryRadius;
    }
    if (Member_bInvulnerable.IsAvailable() && bInvulnerable)
    {
        Rift->bCanBeDamaged = false;
    }
    UGameplayStatics::FinishSpawningActor(Rift, SpawnTransform);
    CachedDeimosSpawner = Rift;
    OnSpawnedRift(Rift);
    // Core's (8.20): the visible knob off hides the rift (the Blueprint's OnSpawnedRift only stops its damage on spawn).
    if (Member_bVisible.IsAvailable() && !bVisible)
    {
        Rift->BlueprintHideRift();
    }
    UE_LOG(LogFortBuilding, Log, TEXT("SpawnRift: %s spawned %s"), *GetName(), *Rift->GetName());

    if (!RequestSettingsLoad(0))
    {
        HandleAsyncLoadComplete();
    }
#endif
}

DEFINE_FUNCTION(ABuildingProp_DeimosSpawner::execSpawnRift)
{
    P_GET_STRUCT(FVector, Z_Param_SpawnPosition);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<ABuildingProp_DeimosSpawner*>(Context)->SpawnRift(Z_Param_SpawnPosition);
    P_NATIVE_END;
}

void ABuildingProp_DeimosSpawner::OnSpawnedRift(ABuildingRift* SpawnedRift)
{
    UFunction* NativeEvent = FindStaticFunction_OnSpawnedRift();
    Reflection::FParams Params(NativeEvent ? FindFunction(NativeEvent->GetFName()) : nullptr, true);
    if (Params.Set(L"SpawnedRift", SpawnedRift))
    {
        Params.Call(this);
    }
}

TAssetSubclassOf<UClass>* ABuildingProp_DeimosSpawner::GetSelectedOption(int32_t Option) const
{
    switch (Option)
    {
    case 0:
        return Member_SelectedPawnNumberCap.IsAvailable() ? &GetMember_SelectedPawnNumberCap() : nullptr;
    case 1:
        return Member_SelectedSpawnGroup.IsAvailable() ? &GetMember_SelectedSpawnGroup() : nullptr;
    case 2:
        return Member_SelectedSpawnTiming.IsAvailable() ? &GetMember_SelectedSpawnTiming() : nullptr;
    case 3:
        return Member_SelectedSpawnLimit.IsAvailable() ? &GetMember_SelectedSpawnLimit() : nullptr;
    default:
        return nullptr;
    }
}

bool ABuildingProp_DeimosSpawner::RequestSettingsLoad(int32_t Option)
{
#if WITH_SERVER_CODE
    // Core's: Epic's load is one FStreamableManager request for all the options, which Core has no way to make; they load
    // through UKismetSystemLibrary::LoadAsset instead, one after another, each finished load resuming
    // HandleAsyncLoadComplete's native with the next option's index. A failed load moves on as a finished one does.
    for (; HandleAsyncLoadCompleteFunction && Option < NumSettingsOptions; ++Option)
    {
        TAssetSubclassOf<UClass>* Selected = GetSelectedOption(Option);
        const FStringAssetReference* Path = Selected ? &Selected->ToStringReference() : nullptr;
        if (!Path || !Path->IsValid())
        {
            continue;
        }
        const FLatentActionInfo LatentInfo{ Option + 1, NextSettingsLoadId++, HandleAsyncLoadCompleteFunction->GetFName(), this };
        if (UKismetSystemLibrary::LoadAsset(this, Path->ToString(), LatentInfo))
        {
            return true;
        }
    }
#endif
    return false;
}

DEFINE_FUNCTION(ABuildingProp_DeimosSpawner::execHandleAsyncLoadComplete)
{
    P_GET_PROPERTY(UIntProperty, Linkage);
    P_FINISH;
    P_NATIVE_BEGIN;
    ABuildingProp_DeimosSpawner* This = static_cast<ABuildingProp_DeimosSpawner*>(Context);
    if (!This->RequestSettingsLoad(Linkage))
    {
        This->HandleAsyncLoadComplete();
    }
    P_NATIVE_END;
}

void ABuildingProp_DeimosSpawner::HandleAsyncLoadComplete()
{
#if WITH_SERVER_CODE
    // The rift's settings from the loaded options, as UFortAsyncAction_MakeTieredWaveEncounterSettings::
    // HandleAsyncLoadComplete (8.20 0xCA9100) makes a wave's: each option class's category option goes into
    // InjectedOverrideCategories (the build's own helper, FFortEncounterSettings::InjectOverrideCategory). The mission
    // copies the settings into each encounter the rift starts; AA_EDO_SpawnGroupProgression_* pick the creatures,
    // AA_EDO_PawnNumberCap_ScriptedSpawn_* how many are out at once, AA_EDO_SpawnTiming_* how soon the next comes and
    // AA_EDO_SpawnLimitPawns_* how many in all.
    AAthenaCreativeRift* Rift = CachedDeimosSpawner;
    if (Role != ROLE_Authority || !Rift || Rift->bHasLoadedSettings)
    {
        return;
    }
    FFortEncounterSettings& Settings = Rift->OverrideEncounterSettings;
    int32_t Injected = 0;
    for (int32_t Option = 0; Option < NumSettingsOptions; ++Option)
    {
        TAssetSubclassOf<UClass>* Selected = GetSelectedOption(Option);
        if (!Selected || !Selected->ToStringReference().IsValid())
        {
            continue;
        }
        UClass* OptionClass = Selected->LoadSynchronous();
        // Core's (8.20): the limit knob's "Infinite" (SetupConfigurables: preset 0, bNoSpawnLimitEnabled) selects
        // AA_EDO_SpawnLimitPawns_0, which leaves the option's PawnNumberLimit at 0. The mission's option set
        // (EDO_Set_Creative_Spawn) limits its encounters by pawn count (SpawnLimitMode NumPawnsLimit, its own
        // PawnLimitCategory that same option), and the encounter's limit check (0xC70540) counts a limit of 0 as reached
        // before the first creature: a headless 8.20 server's creature spawner had its encounter running, its rift ready
        // with 13 spawn slots, bSpawnLimitReached set and no creature. The device's server half that read
        // bNoSpawnLimitEnabled is stripped; "Infinite" gets the encounter's own "no limit", -1 (its constructor's,
        // 0xC6A190), on that option.
        if (Option == 3 && Member_bNoSpawnLimitEnabled.IsAvailable() && bNoSpawnLimitEnabled && OptionClass && OptionClass->IsChildOf(UFortDifficultyOptionEncounter_SpawnLimitPawns::StaticClass()))
        {
            UFortDifficultyOptionEncounter_SpawnLimitPawns* Limit = static_cast<UFortDifficultyOptionEncounter_SpawnLimitPawns*>(OptionClass->GetDefaultObject());
            if (Limit && Limit->PawnNumberLimit == 0)
            {
                Limit->PawnNumberLimit = -1;
            }
        }
        if (OptionClass && Settings.InjectOverrideCategory(OptionClass))
        {
            ++Injected;
        }
        else
        {
            UE_LOG(LogFortBuilding, Warning, TEXT("ABuildingProp_DeimosSpawner::HandleAsyncLoadComplete : Failed to load %s for making encounter settings, it will have no override"),
                *Selected->ToStringReference().ToString());
        }
    }

    // Core's (8.20): the damage-buildings knob is the rifts' own setting; the despawn range ("1 Tile" to "16 Tiles", in
    // cm) is how far from the creatures (bUseDistanceToAI, the "Distance to Enemy" type) or from the rift (the despawn
    // sphere, whose last player out stops the encounter) the players may go.
    if (Member_bDamageBuildingsOnSpawn.IsAvailable())
    {
        Settings.bRiftsDestroyPlayerBuiltBuildings = bDamageBuildingsOnSpawn;
    }
    if (Member_DespawnRangeOverride.IsAvailable() && DespawnRangeOverride > 0.0f)
    {
        USphereComponent* DespawnSphere = Rift->DespawnSphereComponent;
        if (bUseDistanceToAI)
        {
            Settings.AIDespawnDistanceOverride = DespawnRangeOverride;
        }
        else if (DespawnSphere)
        {
            DespawnSphere->SetSphereRadius(DespawnRangeOverride);
        }
    }
    Rift->bHasLoadedSettings = true;

    // Core's (8.20): the activation range knob is the level of the rift's OverlapSphereRadius (BP_DeimosRift_CubeCreative:
    // 512 x Deimos.DynamicSpawner.CreativeSpawningTriggerOptions, whose value is its level, "1 Tile" at 1). Sized once the
    // settings are there, so the players already in range start the encounter.
    USphereComponent* OverlapSphere = Rift->OverlapSphereComponent;
    if (Member_ActivationRangeLevel.IsAvailable() && OverlapSphere)
    {
        const FScalableFloat& Radius = Rift->OverlapSphereRadius;
        OverlapSphere->SetSphereRadius(Radius.GetValueAtLevel(static_cast<float>(ActivationRangeLevel)));
    }
    UE_LOG(LogFortBuilding, Log, TEXT("ABuildingProp_DeimosSpawner::HandleAsyncLoadComplete: %s gave %s %d override categories"), *GetName(), *Rift->GetName(), Injected);
#endif
}

void ABuildingProp_DeimosSpawner::HookVTables()
{
#if WITH_SERVER_CODE
    // 7.40 adds the device (as ABuildingTrapFloor_DeimosSpawner).
    UClass* Class = StaticClass();
    UFunction* MinigameStarted = Class ? FindStaticFunction_MinigameStarted() : nullptr;
    if (!MinigameStarted || (!Natives::FFrame_Step && !Finder::ResolveScriptFrame()))
    {
        return;
    }
    UObject* Default = Class->GetDefaultObject(false);
    void** VTable = Default ? *reinterpret_cast<void***>(Default) : nullptr;
    const int32_t MinigameStartedIndex = Finder::ImplementationIndex(MinigameStarted);
    if (!VTable || MinigameStartedIndex < 0)
    {
        Log::Error(L"Finder: ABuildingProp_DeimosSpawner::MinigameStarted_Implementation slot not found");
        return;
    }
    if (!Finder::IsStubFunction(reinterpret_cast<uintptr_t>(VTable[MinigameStartedIndex])))
    {
        Log::Info(L"Finder: ABuildingProp_DeimosSpawner::MinigameStarted_Implementation is native in this build");
        return;
    }
    if (!AAthenaCreativeRift::HasEncounterSettings() || !UKismetSystemLibrary::HasLoadAsset() || !Finder::ResolveEncounterOverrideCategoryLive())
    {
        Log::Info(L"Hook ABuildingProp_DeimosSpawner::MinigameStarted: not restored, this build's creative rift has no reflected encounter settings, or its override helper isn't found");
        return;
    }

    // A finished UKismetSystemLibrary::LoadAsset resumes its FLatentActionInfo: the callback target's ExecutionFunction
    // with the Linkage as its one int32 parameter, UObject::ExecuteUbergraph's signature.
    UFunction* Signature = UObject::StaticClass()->FindFunctionByName(FName(L"ExecuteUbergraph", FNAME_Find));
    HandleAsyncLoadCompleteFunction = Signature && Signature->ParmsSize == sizeof(int32_t) ? Class->AddNativeFunction(L"HandleAsyncLoadComplete", &execHandleAsyncLoadComplete, Signature) : nullptr;
    if (!HandleAsyncLoadCompleteFunction)
    {
        Log::Error(L"Hook ABuildingProp_DeimosSpawner::HandleAsyncLoadComplete: native not added, so no creature spawner gets its rift");
        return;
    }
    Hooks::SwapVTable(L"ABuildingProp_DeimosSpawner::MinigameStarted", VTable, MinigameStartedIndex, reinterpret_cast<void*>(&MinigameStartedHK));

    UFunction* SpawnRiftFunction = FindStaticFunction_SpawnRift();
    uint8_t* NativeCall = SpawnRiftFunction ? Finder::ImplementationCall(SpawnRiftFunction) : nullptr;
    const bool bSpawnRiftStripped = NativeCall && Finder::IsStubFunction(Finder::RelativeTarget(reinterpret_cast<uintptr_t>(NativeCall), 1, 5));
    if (bSpawnRiftStripped)
    {
        SpawnRiftFunction->SetNativeFunc(&execSpawnRift);
    }
    Log::Info(L"Finder: ABuildingProp_DeimosSpawner::MinigameStarted_Implementation (slot 0x{:X}){} stripped, restored", MinigameStartedIndex,
        bSpawnRiftStripped ? L" and SpawnRift are" : L" is");
#endif
}
