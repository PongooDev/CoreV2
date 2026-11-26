#pragma once

#include "Creative/FortVolume.h"
#include "GameFramework/Info.h"
#include "Reflection.h"

// The island's minigame settings machine (MinigameSettingsMachine_C): the GAME tab's options. From 8.50 an AInfo kept in
// the island record's VolumeInfoActorRecords.
class AFortMinigameSettingsBuilding : public AInfo
{
    DefineUnrealClass(AFortMinigameSettingsBuilding, FortniteGame)
    DefineUFunction(OnRep_SettingsVolume)

public:
    DefineUProperty(UFortCreativeVolumeLinkComponent*, CreativeLinkComponent)
    // 9.00 and later: the machine's volume, replicated (OnRep_SettingsVolume).
    DefineUProperty(AFortVolume*, SettingsVolume)

    static void HookVTables();

private:
    static void BeginPlayHK(AFortMinigameSettingsBuilding* This);
    static inline void (*BeginPlayOG)(AFortMinigameSettingsBuilding*) = nullptr;
};
