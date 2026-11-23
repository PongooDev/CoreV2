#include "pch.h"
#include "Missions/FortCreativeRiftSpawnParams.h"
#include "UObject/Class.h"

bool UFortCreativeRiftSpawnParams::SetParams(ABuildingRift* InRiftSpawn, FFortEncounterSettings* InFortEncounterSettings)
{
    // 7.40 to 8.20 end the class with RiftSpawn and 8 unreflected bytes (0x28 and 0x30 of 0x38), the settings pointer.
    UClass* Class = StaticClass();
    const int32_t SettingsOffset = Member_RiftSpawn.IsAvailable() ? Member_RiftSpawn.GetOffsetChecked() + static_cast<int32_t>(sizeof(void*)) : -1;
    if (!Class || SettingsOffset < 0 || Class->GetPropertiesSize() != SettingsOffset + static_cast<int32_t>(sizeof(void*)))
    {
        return false;
    }
    RiftSpawn = InRiftSpawn;
    *reinterpret_cast<FFortEncounterSettings**>(reinterpret_cast<uint8_t*>(this) + SettingsOffset) = InFortEncounterSettings;
    return true;
}
