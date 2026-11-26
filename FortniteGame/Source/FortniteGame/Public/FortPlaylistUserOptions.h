#pragma once

#include "Containers/UnrealString.h"
#include "UObject/Object.h"

// A playlist's options its players set before the match (from 6.01; Update v6.01's "Playground Custom Options", which
// the game calls matchmaking knobs). Each one is a property (OptionKey) of a class (OptionScope, a soft class: on
// Playground a Creative mutator of the playlist's UserKnobsModifier) with the values the lobby offers
// (UPlaylistUserOptionIntEnum, FloatEnum, IntRange, FloatRange, Bool).
class UPlaylistUserOptionBase : public UObject
{
    DefineUnrealClass(UPlaylistUserOptionBase, FortniteGame)

public:
    DefineUFunction(GetOptionKey)

    // The game's own (CL-4424678 0xEF0A60): OptionKey, and with bWithScoping "<scope>:<OptionKey>", the scope being
    // the short name of OptionScope's package (CreativeMutator_Gravity for /Game/Athena/Playlists/Creative/Mutators/
    // CreativeMutator_Gravity.CreativeMutator_Gravity_C). The scoped key is what the lobby names the option by in the
    // party leader's matchmaking ticket (player.option.custom_match_option.<key>, 0x2E2D0A0), so what the match options
    // have it under. Empty where the build has no such function.
    FString GetOptionKey(bool bWithScoping) const;

    // Whether the option may be left without a value (an empty one clears its override).
    DefineUProperty(bool, bCanHaveNoOverride)
    static bool HasCanHaveNoOverride() { return Member_bCanHaveNoOverride.IsAvailable(); }
    static int32_t CanHaveNoOverrideOffset() { return Member_bCanHaveNoOverride.IsAvailable() ? static_cast<int32_t>(Member_bCanHaveNoOverride.GetOffsetChecked()) : -1; }

    // The game's own (9.10 CL-6573057 0x149ECA0, CL-6639283 0x149F390): the value the option's choice named OptionName
    // stands for (GetOptionValueFromName_Internal, a virtual: an enum option's entry value, a string option's own text),
    // or nothing for an empty name where bCanHaveNoOverride. Empty where the build's wasn't found.
    FString GetOptionValueFromName(const FString& OptionName) const;
};

// UFortPlaylist::UserOptions (6.01's Playlist_Playground: Playgrounds_UserKnobsV1, seven options).
class UPlaylistUserOptions : public UObject
{
    DefineUnrealClass(UPlaylistUserOptions, FortniteGame)

public:
    DefineUProperty(TArray<UPlaylistUserOptionBase*>, Options)

    // The option whose scoped key (GetOptionKey(true)) is OptionKey, ignoring case (9.10 CL-6573057 0x149D080).
    UPlaylistUserOptionBase* GetOption(const FString& OptionKey) const;
};
