#pragma once

#include "Containers/UnrealString.h"
#include "McpProfile.h"
#include "ReflectionParams.h"

// The player's creative profile (from 7.00; profileId=creative): their islands (the CreativePlot items) and its stats,
// last_used_plot among them. The server keeps one per player (AFortPlayerController::CreativeModeProfile).
class UFortMcpProfileCreative : public UMcpProfile
{
    DefineUnrealClass(UFortMcpProfileCreative, FortniteGame)
    DefineUFunction(SetLastUsedCreativePlot)

public:
    // The MCP command that saves PlotItemId as the profile's last_used_plot (a NetRequest UFunction: ProcessEvent sends
    // it, as the game's own wrapper does, 7.10 0x1608550). Its FDedicatedServerUrlContext is the one the struct's
    // constructor makes (McpDedicatedServerCommandUrl, the server's credentials), as InitializeStruct leaves it: 7.10's
    // ServerLoadPlotForPortal_Implementation passes a fresh one. False on a build without the command.
    bool SetLastUsedCreativePlot(const FString& PlotItemId)
    {
        UFunction* Function = FindStaticFunction_SetLastUsedCreativePlot();
        Reflection::FParams Params(Function, true);
        return Function && Params.Set(L"PlotItemId", PlotItemId) && Params.Call(this);
    }
};
