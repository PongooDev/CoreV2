#include "pch.h"
#include "Camera/FortPlayerCameraZone.h"
#include "Containers/Set.h"
#include "Finder.h"
#include "Hooks.h"
#include "Log.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

void AFortPlayerCameraZone::HookVTables()
{
#if WITH_SERVER_CODE
    // CoreV2 #567. The client build's AFortPlayerCameraZone wraps the four camera cache accessors in its anti-cheat
    // (UAC call counters, AntiCheatData): the setter scrambles the POV's location, rotation and FOV into one static
    // block per kind (8.20 0x5DE2660, 50 bytes each, scheme byte 0x5DCD1F0; 8.51 0x58CE820 + 48 * kind) and the getter
    // unscrambles them from there, not from the camera manager. One block serves every camera manager, which is right
    // for a client's one local camera but not for a server's camera per player: each ServerUpdateCamera
    // (0x2C1EAC0 -> FillCameraCache 0x2C0F210) overwrites every player's cache, so GetPlayerViewPoint (0x2C134A0 ->
    // GetCameraViewPoint 0x138F830 -> GetCameraCachePOV 0x138F5E0) and with it the replication graph's view point
    // and the players' aim are the last player's camera. Seen live on 8.20: both connections' FNetViewer at the
    // same point every frame. 8.20 (0x40A9218), 8.51 (0x41A6BD8) and 9.00 (0x42D1598) override all four. The server
    // keeps each manager's cache in its own CameraCachePrivate / LastFrameCameraCachePrivate, with
    // APlayerCameraManager's accessors. The view target's POV (kind 2) and the one at slot 0x650 (kind 3) go through
    // the same blocks but are only written by UpdateCamera, which a server doesn't run for remote players.
    const int32_t CacheIndex = Finder::CameraCachePOVIndex();
    UClass* const ZoneClass = CacheIndex >= 0 ? StaticClass() : nullptr;
    UObject* const ManagerDefault = ZoneClass && APlayerCameraManager::StaticClass() ? APlayerCameraManager::StaticClass()->GetDefaultObject(false) : nullptr;
    void** const ManagerVTable = ManagerDefault ? *reinterpret_cast<void***>(ManagerDefault) : nullptr;
    if (!ManagerVTable)
    {
        return;
    }

    static constexpr const wchar_t* Accessors[] = {
        L"AFortPlayerCameraZone::SetCameraCachePOV",
        L"AFortPlayerCameraZone::SetLastFrameCameraCachePOV",
        L"AFortPlayerCameraZone::GetCameraCachePOV",
        L"AFortPlayerCameraZone::GetLastFrameCameraCachePOV",
    };
    TSet<void**> Restored;
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* const Class = *It;
        UObject* const ClassDefault = Class->IsChildOf(ZoneClass) ? Class->GetDefaultObject(false) : nullptr;
        void** const VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (!VTable || Restored.Contains(VTable))
        {
            continue;
        }
        Restored.Add(VTable);
        for (int32_t Accessor = 0; Accessor < 4; ++Accessor)
        {
            const int32_t Index = CacheIndex + Accessor;
            if (VTable[Index] != ManagerVTable[Index])
            {
                Hooks::SwapVTable(Accessors[Accessor], VTable, Index, ManagerVTable[Index]);
            }
        }
    }
#endif
}
