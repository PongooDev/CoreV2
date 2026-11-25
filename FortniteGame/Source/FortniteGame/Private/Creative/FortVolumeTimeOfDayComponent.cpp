#include "pch.h"
#include "Creative/FortVolumeTimeOfDayComponent.h"
#include "Athena/Mutators/FortMutatorListComponent.h"
#include "Creative/FortVolume.h"
#include "Engine/World.h"
#include "Finder.h"
#include "FortGameState.h"
#include "Hooks.h"
#include "Kismet/GameplayStatics.h"
#include "Log.h"
#include "Logging/LogMacros.h"
#include "Math/UnrealMathUtility.h"
#include "Natives.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/Stack.h"
#include "UObject/UObjectIterator.h"

#include <algorithm>

DEFINE_LOG_CATEGORY_STATIC(LogFortCreative, Log, All);

void AFortTimeOfDayManager::SetTimeOfDayFloat(float TimeOfDayInHours)
{
    Reflection::FParams Params(FindStaticFunction_SetTimeOfDayFloat());
    if (Params.Set(L"TimeOfDayInHours", TimeOfDayInHours))
    {
        Params.Call(this);
    }
}

void AFortTimeOfDayManager::SetTimeOfDaySpeedFloat(float InTimeOfDaySpeed)
{
    Reflection::FParams Params(FindStaticFunction_SetTimeOfDaySpeedFloat());
    if (Params.Set(L"InTimeOfDaySpeed", InTimeOfDaySpeed))
    {
        Params.Call(this);
    }
}

void UFortVolumeTimeOfDayComponent::BeginPlay()
{
    BeginPlayOG(this);

#if WITH_SERVER_CODE
    AActor* const Volume = GetOwner();
    if (!Volume || Volume->Role != ROLE_Authority)
    {
        return;
    }
    const TArray<UActorComponent*> Lists = Volume->GetComponentsByClass(UFortMutatorListComponent::StaticClass());
    UFortMutatorListComponent* const MutatorList = Lists.Num() > 0 ? static_cast<UFortMutatorListComponent*>(Lists[0]) : nullptr;
    if (!MutatorList)
    {
        return;
    }
    const FName Handler = FindStaticFunction_MutatorSettingsChanged()->GetFName();
    FMulticastScriptDelegate& SettingsInitialized = MutatorList->OnSettingsInitialized;
    FMulticastScriptDelegate& SettingsChanged = MutatorList->OnSettingsChanged;
    SettingsInitialized.AddUniqueDynamic(this, Handler);
    SettingsChanged.AddUniqueDynamic(this, Handler);
    // The island's settings may already be in its mutators.
    MutatorSettingsChanged(MutatorList->PropertyOverrideArray);
#endif
}

void UFortVolumeTimeOfDayComponent::MutatorSettingsChanged(const FPropertyOverrideData& OverrideData)
{
#if WITH_SERVER_CODE
    AActor* const Volume = GetOwner();
    if (!Volume || Volume->Role != ROLE_Authority)
    {
        return;
    }
    const TArray<UActorComponent*> Lists = Volume->GetComponentsByClass(UFortMutatorListComponent::StaticClass());
    const UFortMutatorListComponent* const MutatorList = Lists.Num() > 0 ? static_cast<UFortMutatorListComponent*>(Lists[0]) : nullptr;
    AFortAthenaMutator_TODOverride* const TODMutator = MutatorList
        ? Cast<AFortAthenaMutator_TODOverride>(MutatorList->GetMutatorByClass(AFortAthenaMutator_TODOverride::StaticClass())) : nullptr;
    if (!TODMutator)
    {
        return;
    }

    const uint8_t Override = TODMutator->TimeOfDayOverride;
    const uint8_t Hour0 = EAthenaTimeOfDayOverride::GetHour0();
    const uint8_t Hour23 = EAthenaTimeOfDayOverride::GetHour23();
    const bool bRandom = Override == EAthenaTimeOfDayOverride::GetRandom();
    FFortVolumeTimeOfDayConfig& Config = TimeOfDayConfig;
    if (Override == EAthenaTimeOfDayOverride::GetNoOverride())
    {
        if (Config.bShouldOverrideTimeOfDay)
        {
            Config.bShouldOverrideTimeOfDay = false;
            Config.bUseRandomTimeOfDay = false;
            Volume->FlushNetDormancy();
            UE_LOG(LogFortCreative, Log, TEXT("MutatorSettingsChanged: %s follows the world's time of day again"), *Volume->GetName());
        }
        return;
    }

    // The knob's hours and Random are a fixed time ("in hours or random: fixed time"); Custom, ForceDay and ForceNight,
    // which no Creative knob sets, take the mutator's own TimeOverride and SpeedOverride.
    float Hours = TODMutator->TimeOverride;
    float Speed = TODMutator->SpeedOverride;
    if (Override >= Hour0 && Override <= Hour23)
    {
        Hours = static_cast<float>(Override - Hour0);
        Speed = 0.0f;
    }
    else if (bRandom)
    {
        Hours = static_cast<float>(std::min(static_cast<int32_t>(FMath::FRand() * 24.0f), 23));
        Speed = 0.0f;
    }

    // A manager the island's clear destroyed (below) is spawned again.
    AFortTimeOfDayManager* Manager = TimeOfDayManager;
    if (!Manager || Manager->IsPendingKill())
    {
        // The island's own manager is one of the world's class (AFortTimeOfDayManager's constructor makes it always
        // relevant and replicated), where the world's is. Not at the island: the volume manager registers every actor
        // spawned inside a volume with it (HandleOnActorSpawned, 8.51 0x1179200, by location), and switching the
        // island destroys the volume's AInfo actors with the rest (ServerClearVolume_Implementation 0x1188FA0 ->
        // 0x116D730), which left the component on a dead manager (#646).
        UWorld* const World = GetWorld();
        AFortGameState* const GameState = World ? Cast<AFortGameState>(World->GetGameState()) : nullptr;
        AFortTimeOfDayManager* const WorldManager = GameState ? GameState->FortTimeOfDayManager : nullptr;
        if (!WorldManager)
        {
            UE_LOG(LogFortCreative, Warning, TEXT("MutatorSettingsChanged: %s has no world time of day manager to copy"), *Volume->GetName());
            return;
        }
        const FTransform SpawnTransform = WorldManager->GetTransform();
        Manager = Cast<AFortTimeOfDayManager>(UGameplayStatics::BeginDeferredActorSpawnFromClass(this, WorldManager->GetClass(), SpawnTransform,
            ESpawnActorCollisionHandlingMethod::AlwaysSpawn, Volume));
        if (!Manager)
        {
            UE_LOG(LogFortCreative, Error, TEXT("MutatorSettingsChanged: %s couldn't spawn its %s"), *Volume->GetName(), *WorldManager->GetClass()->GetName());
            return;
        }
        UGameplayStatics::FinishSpawningActor(Manager, SpawnTransform);
        TimeOfDayManager = Manager;
    }

    Manager->SetTimeOfDayFloat(Hours);
    Manager->SetTimeOfDaySpeedFloat(Speed);
    Config.bShouldOverrideTimeOfDay = true;
    Config.bUseRandomTimeOfDay = bRandom;
    Volume->FlushNetDormancy();
    UE_LOG(LogFortCreative, Log, TEXT("MutatorSettingsChanged: %s's time of day is %.1f h (speed %.2f, override %d) on %s"), *Volume->GetName(), Hours, Speed,
        static_cast<int32_t>(Override), *Manager->GetName());
#endif
}

DEFINE_FUNCTION(UFortVolumeTimeOfDayComponent::execMutatorSettingsChanged)
{
    // The delegate's parameter struct is the frame's Locals (ProcessEvent's Parms); OverrideData is its only member, and
    // MutatorSettingsChanged reads the mutators, not it.
    const FPropertyOverrideData* const OverrideData = Natives::FFrame_Locals >= 0 ? reinterpret_cast<const FPropertyOverrideData*>(P_STACK.Locals) : nullptr;
    P_FINISH;
    P_NATIVE_BEGIN;
    if (OverrideData)
    {
        static_cast<UFortVolumeTimeOfDayComponent*>(Context)->MutatorSettingsChanged(*OverrideData);
    }
    P_NATIVE_END;
}

void UFortVolumeTimeOfDayComponent::BeginPlayHK(UFortVolumeTimeOfDayComponent* This)
{
    This->BeginPlay();
}

void UFortVolumeTimeOfDayComponent::HookVTables()
{
#if WITH_SERVER_CODE
    UClass* const Class = StaticClass();
    UFunction* const Handler = Class ? FindStaticFunction_MutatorSettingsChanged() : nullptr;
    if (!Handler)
    {
        return;
    }
    if (!Member_TimeOfDayManager.IsAvailable() || !Member_TimeOfDayConfig.IsAvailable() || !AFortTimeOfDayManager::IsAvailable()
        || !AFortAthenaMutator_TODOverride::IsAvailable() || !UFortMutatorListComponent::HasSettingsChanged()
        || !UFortMutatorListComponent::CanGetMutatorByClass() || !EAthenaTimeOfDayOverride::StaticEnum())
    {
        Log::Error(L"Finder: UFortVolumeTimeOfDayComponent's island time of day not restored (a member, the time of day manager's setters, the TOD mutator or the mutator list's settings delegates are missing)");
        return;
    }

    // BeginPlay is the first of the two adjacent UActorComponent slots it overrides (EndPlay the second; 8.00 and 8.20
    // 0x60/0x61, 8.30 and 8.40 0x5F/0x60) whose override starts by calling UActorComponent's.
    UObject* const ComponentDefault = UActorComponent::StaticClass()->GetDefaultObject(false);
    UObject* const Default = Class->GetDefaultObject(false);
    void** const ComponentVTable = ComponentDefault ? *reinterpret_cast<void***>(ComponentDefault) : nullptr;
    void** const VTable = Default ? *reinterpret_cast<void***>(Default) : nullptr;
    const auto IsCode = [](const void* Address)
    {
        return reinterpret_cast<uintptr_t>(Address) >= Finder::TextBegin() && reinterpret_cast<uintptr_t>(Address) < Finder::TextEnd();
    };
    int32_t BeginPlayIndex = -1;
    for (int32_t Index = 1; ComponentVTable && VTable && Index + 1 < 0x100 && IsCode(ComponentVTable[Index + 1]); ++Index)
    {
        if (VTable[Index] == ComponentVTable[Index] || VTable[Index + 1] == ComponentVTable[Index + 1] || !IsCode(VTable[Index]))
        {
            continue;
        }
        const std::vector<std::pair<uintptr_t, uintptr_t>> Calls = Finder::DecodedDirectCalls(reinterpret_cast<uintptr_t>(VTable[Index]));
        // 8.00 (CL-5203069, slot 0x60 0xFDC1E0): no replay binding yet, so BeginPlay is only a jmp to UActorComponent's.
        const uint8_t* const Code = reinterpret_cast<const uint8_t*>(VTable[Index]);
        const bool bForwards = Code[0] == 0xE9
            && reinterpret_cast<uintptr_t>(Code) + 5 + *reinterpret_cast<const int32_t*>(Code + 1) == reinterpret_cast<uintptr_t>(ComponentVTable[Index]);
        if (bForwards || (!Calls.empty() && Calls.front().second == reinterpret_cast<uintptr_t>(ComponentVTable[Index])))
        {
            BeginPlayIndex = Index;
            break;
        }
    }
    if (BeginPlayIndex < 0)
    {
        Log::Error(L"Finder: UFortVolumeTimeOfDayComponent::BeginPlay not found, so the islands' Time of Day setting does nothing");
        return;
    }

    Handler->SetNativeFunc(&execMutatorSettingsChanged);
    void* const Original = VTable[BeginPlayIndex];
    BeginPlayOG = reinterpret_cast<decltype(BeginPlayOG)>(Original);
    int32_t Swapped = 0;
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UObject* const ClassDefault = It->IsChildOf(Class) ? It->GetDefaultObject(false) : nullptr;
        void** const ClassVTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (ClassVTable && ClassVTable[BeginPlayIndex] == Original)
        {
            Hooks::SwapVTable(L"UFortVolumeTimeOfDayComponent::BeginPlay", ClassVTable, BeginPlayIndex, reinterpret_cast<void*>(&BeginPlayHK));
            ++Swapped;
        }
    }
    Log::Info(L"Finder: UFortVolumeTimeOfDayComponent's server half is stripped: BeginPlay (slot 0x{:X}, 0x{:X}) binds MutatorSettingsChanged to the island's mutator list again (in {} classes), its body restored",
        BeginPlayIndex, reinterpret_cast<uintptr_t>(Original) - Finder::ImageBase(), Swapped);
#endif
}
