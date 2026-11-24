#pragma once

#include "Reflection.h"
#include "UObject/Object.h"

class AFortPlayerController;
struct FGameplayTagContainer;

// What a Creative game counts (8.51: MinigameStat_AIKills, Kill of a Pawn.Athena.NPC.Deimos target; MinigameStat_Score,
// MinigameDynamicEvent with Minigame.Stat.Score). AFortMinigame::TrackedStats lists the classes; NotifyStatEvent asks
// each one's default object whether a stat event counts for it.
class UFortMinigameStatFilter : public UObject
{
    DefineUnrealClass(UFortMinigameStatFilter, FortniteGame)

public:
    DefineUFunction(Matches)
    // A BlueprintNativeEvent: called through ProcessEvent, so a filter Blueprint's override answers.
    bool Matches(uint8_t InStatEvent, UObject* InTargetObject, AFortPlayerController* InPlayerController, const FGameplayTagContainer& InTargetTags,
        const FGameplayTagContainer& InSourceTags, const FGameplayTagContainer& InContextTags) const;
};
