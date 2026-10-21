#pragma once

#include "ObjectLayout.h"
#include "UObject/Object.h"

namespace ENavigationBuildLock
{
    enum Type
    {
        NoUpdateInEditor = 1 << 1,
        InitialLock = 1 << 2,
        Custom = 1 << 3,
    };
}

class UNavigationSystem : public UObject
{
    DefineUnrealClass(UNavigationSystem, Engine)
    DefineNativeMember(uint8_t, NavBuildingLockFlags, UNavigationSystem_NavBuildingLockFlags)

public:
    bool IsNavigationBuildingLocked() const { return NavBuildingLockFlags != 0; }
    void RemoveNavigationBuildLock(uint8_t Flags);
    void ReleaseInitialBuildingLock();
    void RebuildAll(bool bIsLoadTime = false);
};
