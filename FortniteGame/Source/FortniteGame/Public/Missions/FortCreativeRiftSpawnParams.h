#pragma once

#include "UObject/Object.h"

class ABuildingRift;
struct FFortEncounterSettings;

// The params of the "RiftCreated" mission event a Creative creature spawner's rift sends as it begins play (7.40 adds
// the class): the rift and its encounter settings. AFortMission_RiftSpawners takes the rift into its spawners with them.
class UFortCreativeRiftSpawnParams : public UObject
{
    DefineUnrealClass(UFortCreativeRiftSpawnParams, FortniteGame)

public:
    DefineUProperty(ABuildingRift*, RiftSpawn)
    DefineUProperty(bool, bResumeRift)

    static bool ResolveSetParams(uintptr_t BeginPlay);
    // Native only (8.20 0x10BC670): the rift and a pointer to its settings, the member 21.40's layout has right after
    // RiftSpawn (FortEncounterSettings), which nothing reflects. False where the class isn't laid out that way.
    bool SetParams(ABuildingRift* InRiftSpawn, FFortEncounterSettings* InFortEncounterSettings);
};
