#pragma once

#include "Athena/Mutators/FortMutatorListComponent.h"
#include "Components/ActorComponent.h"
#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "Reflection.h"
#include "UObject/ScriptDelegates.h"

class UPlaylistUserOptions;

// The options panel of a placed Creative device or prop (7.00 on): PlayerOptions lists its options, PlayerOptionData
// holds the values set and puts them into the owner's properties, and OnAnyPropertyChanged tells the owner's Blueprint.
// A player's change arrives as AFortPlayerController::ServerUpdateActorOptions, whose intact _Implementation calls
// SetPropertyOverrides.
class UFortActorOptionsComponent : public UActorComponent
{
    DefineUnrealClass(UFortActorOptionsComponent, FortniteGame)
    DefineUFunction(SetPropertyOverrides)
    DefineUFunction(SetPropertyOverrideValues)

public:
    DefineUProperty(UPlaylistUserOptions*, PlayerOptions)
    DefineUStructProperty(FPropertyOverrideData, PlayerOptionData)
    DefineUProperty(FMulticastScriptDelegate, OnAnyPropertyChanged)

    // NewOptions by scoped option key, each value as the option's choice names it: the ones PlayerOptions knows are
    // turned into their values, then SetPropertyOverrideValues.
    bool SetPropertyOverrides(const TMap<FString, FString>& NewOptions);
    // Puts the values into PlayerOptionData, resaves the owner into its island's record, and broadcasts
    // OnAnyPropertyChanged.
    bool SetPropertyOverrideValues(const TMap<FString, FString>& NewOptions);

    // 9.10's options: see the .cpp.
    static void HookCallSites();

private:
    static bool SetPropertyOverridesCall(UFortActorOptionsComponent* This, const TMap<FString, FString>& NewOptions);
    static bool SetPropertyOverrideValuesCall(UFortActorOptionsComponent* This, const TMap<FString, FString>& NewOptions);
};
