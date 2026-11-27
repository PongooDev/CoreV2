#include "pch.h"
#include "Missions/FortCreativeRiftSpawnParams.h"
#include "Finder.h"
#include "Log.h"
#include "UObject/Class.h"

namespace
{
    void (*SetParamsWithResume)(UFortCreativeRiftSpawnParams*, ABuildingRift*, FFortEncounterSettings*, bool) = nullptr;
}

bool UFortCreativeRiftSpawnParams::ResolveSetParams(uintptr_t BeginPlay)
{
    if (!Member_bResumeRift.IsAvailable())
    {
        return true;
    }
    if (!Member_RiftSpawn.IsAvailable())
    {
        return false;
    }
    const int32_t Rift = Member_RiftSpawn.GetOffsetChecked();
    const int32_t Settings = Rift + static_cast<int32_t>(sizeof(void*));
    const int32_t Resume = Member_bResumeRift.GetOffsetChecked();
    if (Rift < 0 || Resume != Settings + static_cast<int32_t>(sizeof(void*)) || Resume > 0x7F)
    {
        return false;
    }

    // BeginPlay calls the native four-argument SetParams in 9.20/9.21 (0x1692B40/0x16842C0).
    // It stores rdx, r8 and r9b into the three fields, then returns.
    for (const auto& [Site, Target] : Finder::DecodedDirectCalls(BeginPlay))
    {
        if (Finder::BodyIs(Target, { 0x48, 0x89, 0x51, static_cast<uint8_t>(Rift), 0x4C, 0x89, 0x41, static_cast<uint8_t>(Settings),
                                     0x44, 0x88, 0x49, static_cast<uint8_t>(Resume), 0xC3 }))
        {
            SetParamsWithResume = reinterpret_cast<decltype(SetParamsWithResume)>(Target);
            Log::Info(L"Finder: UFortCreativeRiftSpawnParams::SetParams with bResumeRift +0x{:X}", Target - Finder::ImageBase());
            return true;
        }
    }
    return false;
}

bool UFortCreativeRiftSpawnParams::SetParams(ABuildingRift* InRiftSpawn, FFortEncounterSettings* InFortEncounterSettings)
{
    if (SetParamsWithResume)
    {
        SetParamsWithResume(this, InRiftSpawn, InFortEncounterSettings, false);
        return true;
    }
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
