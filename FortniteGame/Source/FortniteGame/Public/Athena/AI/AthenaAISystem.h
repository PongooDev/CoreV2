#pragma once

#include "Natives.h"
#include "UObject/Object.h"

class UWorld;

// From 6.x, the AI perception of Battle Royale's AI (6.20's Fortnitemares Cube Monsters): each tick (6.20 CL-4497486
// 0xAAC8F0, "STAT_DeimosAI_PerceptionManagerTick") every player controller's moving pawn makes a footstep noise for the
// AI's hearing (UAthenaAISense_Hearing), as loud as its speed over MaxPlayerSpeedScaleFootstepSounds, between
// MinFootstepHearingRange and MaxFootstepHearingRange (crouched: times CrouchHearingModifier). The four come from the
// match's UAthenaAISettings (OnAISettingsUpdated, 21.40's name; 6.20 0xA9DEA0). Left at the constructor's -1 (0xA7FB10)
// the noise is never louder than 0 and none is made, so the AI hears no player.
class UAthenaAIPerceptionManager : public UObject
{
    DefineUnrealClass(UAthenaAIPerceptionManager, FortniteGame)

public:
    // The game's: the four footstep settings from Settings (a UAthenaAISettings).
    void OnAISettingsUpdated(const UObject* Settings)
    {
        if (Natives::UAthenaAIPerceptionManager_OnAISettingsUpdated)
        {
            Natives::UAthenaAIPerceptionManager_OnAISettingsUpdated(this, Settings);
        }
    }
};

// From 6.x, the AI system of an Athena game mode's world (AFortWorldSettings::GetAISystemClassName, 6.20 0xFF0050).
// Its StartPlay (6.20 0xAABEA0, as the world begins play) hands the game mode's AISettings to its perception manager.
class UAthenaAISystem : public UObject
{
    DefineUnrealClass(UAthenaAISystem, FortniteGame)

public:
    DefineUProperty(UAthenaAIPerceptionManager*, PerceptionManager)

    // The world's AI system when it is a UAthenaAISystem, else null (builds before 6.x, Save the World's worlds).
    static UAthenaAISystem* Get(const UWorld* World);
    // Where the match's AI settings arrive after the world began play (Core's playlist steps, after the map loaded),
    // they reach the perception manager as StartPlay would have given them (21.40's UAthenaAISystem::OnAISettingsUpdated).
    void OnAISettingsUpdated(const UObject* Settings);
};
