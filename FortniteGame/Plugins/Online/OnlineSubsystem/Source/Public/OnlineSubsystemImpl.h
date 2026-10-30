#pragma once

#include "CoreGlobals.h"
#include "ObjectLayout.h"

#include <atomic>

class FOnlineSubsystemImpl
{
public:
    DefineNativeMember(bool, bForceDedicated, FOnlineSubsystemImpl_bForceDedicated)

    bool IsDedicated() const
    {
        return bForceDedicated || IsRunningDedicatedServer();
    }

    static void Hook();

    // For a packed build, whose detours wait until the engine has loaded (Packed::RunBeforeEngineInit): IsDedicated's
    // entries in the online subsystems' vtables, data the packer's check doesn't look at, swapped as soon as the stub
    // hands over. The online subsystem starts during PreInit and makes its voice interface there, as a server's (which
    // forwards voice and needs no voice engine) only if it already answers that it is dedicated.
    static void HookVTables();

    // Run once, on the thread that first asks whether the subsystem is dedicated (the engine's, as the online subsystem
    // starts during PreInit), before it gets the answer.
    static inline std::atomic<void (*)()> BeforeFirstDedicatedCheck = nullptr;

    // Packed builds only (set by HookVTables): the subsystems that have asked whether they are dedicated, the first
    // MaxSubsystems of them, empty slots null. It's how Core finds the MCP subsystem once its hooks are in, whose
    // services read their configs during PreInit (FFortMcpConfig::AddDedicatedServerPermissionsToLoadedServices).
    static constexpr int32_t MaxSubsystems = 8;
    static inline std::atomic<const FOnlineSubsystemImpl*> Subsystems[MaxSubsystems] = {};

private:
    static inline bool bRecordSubsystems = false;

    static void Remember(const FOnlineSubsystemImpl* Subsystem);
    static bool IsDedicatedHK(const FOnlineSubsystemImpl* This);
};
