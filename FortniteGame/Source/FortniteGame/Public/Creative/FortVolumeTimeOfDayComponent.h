#pragma once

#include "Athena/Mutators/FortAthenaMutator.h"
#include "Components/ActorComponent.h"
#include "GameFramework/Info.h"
#include "Reflection.h"
#include "UObject/Stack.h"

// The time of day a Creative island's "Time of Day" setting picks (TimeOfDayV2_UserKnob writes it into the island's
// CreativeMutator_TimeOfDay): NoOverride ("Default"), Hour0 to Hour23 and Random. 8.00+ (CL-5203069).
class EAthenaTimeOfDayOverride
{
    DefineUnrealEnum(EAthenaTimeOfDayOverride, FortniteGame)
    DefineUEnumValue(NoOverride)
    DefineUEnumValue(Custom)
    DefineUEnumValue(Hour0)
    DefineUEnumValue(Hour23)
    DefineUEnumValue(Random)
};

// The sky, sun and fog of a world, or of one Creative island (UFortVolumeTimeOfDayComponent's own).
class AFortTimeOfDayManager : public AInfo
{
    DefineUnrealClass(AFortTimeOfDayManager, FortniteGame)
    DefineUFunction(SetTimeOfDayFloat)
    DefineUFunction(SetTimeOfDaySpeedFloat)

public:
    // BlueprintAuthorityOnly: the time in hours, [0, 24) (8.40 0x1330420 sets TimeOfDay and the replicated
    // TimeOfDayReplicated), and how fast it goes (0x1330980, TimeOfDaySpeed replicates; 0 stops it).
    void SetTimeOfDayFloat(float TimeOfDayInHours);
    void SetTimeOfDaySpeedFloat(float InTimeOfDaySpeed);

    static bool IsAvailable() { return StaticClass() && FindStaticFunction_SetTimeOfDayFloat() && FindStaticFunction_SetTimeOfDaySpeedFloat(); }
};

// The island's time of day mutator (CreativeMutator_TimeOfDay_C, in CreativeIslandMutatorList): the setting lands in
// TimeOfDayOverride. It has no code of its own (8.20's vtable is AFortAthenaMutator's but for its destructor); the
// island's UFortVolumeTimeOfDayComponent reads it.
class AFortAthenaMutator_TODOverride : public AFortAthenaMutator
{
    DefineUnrealClass(AFortAthenaMutator_TODOverride, FortniteGame)

public:
    DefineUProperty(uint8_t, TimeOfDayOverride)
    DefineUProperty(float, TimeOverride)
    DefineUProperty(float, SpeedOverride)

    static bool IsAvailable() { return StaticClass() && Member_TimeOfDayOverride.IsAvailable() && Member_TimeOverride.IsAvailable() && Member_SpeedOverride.IsAvailable(); }
};

struct FFortVolumeTimeOfDayConfig
{
    DefineUnrealStruct(FFortVolumeTimeOfDayConfig, FortniteGame)
    DefineBitfieldUProperty(bShouldOverrideTimeOfDay)
    DefineBitfieldUProperty(bUseRandomTimeOfDay)
};

struct FPropertyOverrideData;

// A Creative island's own time of day (8.00+, on FortVolumeActor_Base_C). The clients do their half from what it
// replicates: OnRep_TimeOfDayManager binds ClientEnteredVolume and ClientExitedVolume, and while
// TimeOfDayConfig.bShouldOverrideTimeOfDay is on, a player on the island sees TimeOfDayManager instead of the world's
// (8.40 0x1325EE0 OnRep_TimeOfDayConfig, 0x1333AD0 the switch). The server's half is stripped (8.20, 8.30 and 8.40 checked):
// BeginPlay (8.40 slot 0x5F, 0x1315CE0) has only its replay spectator binding, MutatorSettingsChanged's exec calls the
// shared empty function, and nothing in the image names MutatorSettingsChanged for a binding.
class UFortVolumeTimeOfDayComponent : public UActorComponent
{
    DefineUnrealClass(UFortVolumeTimeOfDayComponent, FortniteGame)
    DefineUFunction(MutatorSettingsChanged)

public:
    DefineUProperty(AFortTimeOfDayManager*, TimeOfDayManager)
    DefineUStructProperty(FFortVolumeTimeOfDayConfig, TimeOfDayConfig)

    void BeginPlay();
    // Bound to the island's UFortMutatorListComponent OnSettingsChanged, which its SetPropertyOverrides broadcasts once it
    // has put the island's settings into its mutators (AFortPlayerController::ServerUpdateGameplayOptions, the island
    // settings menu). Sets the island's time of day from its AFortAthenaMutator_TODOverride.
    void MutatorSettingsChanged(const FPropertyOverrideData& OverrideData);

    static void HookVTables();

private:
    DECLARE_FUNCTION(execMutatorSettingsChanged);

    static inline void (*BeginPlayOG)(UFortVolumeTimeOfDayComponent* This) = nullptr;
    static void BeginPlayHK(UFortVolumeTimeOfDayComponent* This);
};
