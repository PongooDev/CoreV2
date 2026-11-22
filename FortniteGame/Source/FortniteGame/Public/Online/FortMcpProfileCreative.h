#pragma once

#include "Containers/UnrealString.h"
#include "Delegates/Delegate.h"
#include "McpProfile.h"
#include "ReflectionParams.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

struct FMcpQueryResult;

// The player's creative profile (from 7.00; profileId=creative): their islands (the CreativePlot items) and its stats,
// last_used_plot among them. The server keeps one per player (AFortPlayerController::CreativeModeProfile).
class UFortMcpProfileCreative : public UMcpProfile
{
    DefineUnrealClass(UFortMcpProfileCreative, FortniteGame)
    DefineUFunction(SetLastUsedCreativePlot)
    DefineUFunction(SetCreativePlotMetadata)

public:
    // FBaseUrlContext's Callback, TDelegate<void(const FMcpQueryResult&)>: the command's answer.
    using FOnQueryComplete = TBaseDelegate<void, const FMcpQueryResult&>;

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

    // 7.40's (CL-5046157) command for an island's description (dedicated server). 7.40 has no wrapper for it (only its
    // FName's initialiser, 0x148BC6, is left), so it is sent the way the game's wrappers send the others
    // (ModifyCreativePlotPermissions, 0x174F650: the context copied into the parameters, then ProcessEvent), with
    // Callback in the context. From 4.2 the context is 0x38 bytes and its Callback (a heap FDelegateBase) is at +8 (7.40's
    // permissions request, 0x10C35F0, copies its delegate there). False when the command isn't sent.
    bool SetCreativePlotMetadata(const FString& PlotItemId, const FString& Locale, const FString& Title, const FString& Tagline, const FString& YoutubeVideoId,
        const FString& Introduction, FOnQueryComplete& Callback)
    {
        constexpr int32_t ContextSize = 0x38;
        constexpr int32_t CallbackOffset = 0x08;
        UFunction* Function = FindStaticFunction_SetCreativePlotMetadata();
        UProperty* ContextProperty = Function ? Function->FindPropertyByName(FName(L"Context", FNAME_Find)) : nullptr;
        if (!ContextProperty || ContextProperty->ElementSize != ContextSize || !DelegateBase::UsesHeapAllocator())
        {
            return false;
        }
        Reflection::FParams Params(Function, true);
        if (!Params.Set(L"PlotItemId", PlotItemId) || !Params.Set(L"Locale", Locale) || !Params.Set(L"Title", Title) || !Params.Set(L"Tagline", Tagline)
            || !Params.Set(L"YoutubeVideoId", YoutubeVideoId) || !Params.Set(L"Introduction", Introduction))
        {
            return false;
        }
        uint8_t* Context = static_cast<uint8_t*>(Params.GetAddress(L"Context"));
        Callback.RelocateTo(Context + CallbackOffset);
        return Params.Call(this);
    }
};
