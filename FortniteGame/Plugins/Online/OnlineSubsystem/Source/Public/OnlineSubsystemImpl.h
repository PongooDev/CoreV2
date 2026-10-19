#pragma once

#include "CoreGlobals.h"
#include "ObjectLayout.h"

class FOnlineSubsystemImpl
{
public:
    DefineNativeMember(bool, bForceDedicated, FOnlineSubsystemImpl_bForceDedicated)

    bool IsDedicated() const
    {
        return bForceDedicated || IsRunningDedicatedServer();
    }

    static void Hook();

private:
    static bool IsDedicatedHK(const FOnlineSubsystemImpl* This);
};
