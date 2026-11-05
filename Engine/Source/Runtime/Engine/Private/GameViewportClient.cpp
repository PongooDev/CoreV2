#include "pch.h"
#include "Engine/GameViewportClient.h"
#include "CoreGlobals.h"
#include "Engine/Engine.h"
#include "Hooks.h"
#include "Log.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Natives.h"
#include "Templates/Casts.h"
#include "UObject/UObjectGlobals.h"

UEngine* UGameViewportClient::GetOuterUEngine() const
{
    return static_cast<UEngine*>(GetOuter());
}

ULocalPlayer* UGameViewportClient::SetupInitialLocalPlayer(FString& OutError)
{
    check(GetOuterUEngine()->ConsoleClass != nullptr);

#if ALLOW_CONSOLE
    ViewportConsole = NewObject<UConsole>(this, GetOuterUEngine()->ConsoleClass);
    GLog->AddOutputDevice(ViewportConsole);
#endif

    return SetupInitialLocalPlayerOG(this, OutError);
}

ULocalPlayer* UGameViewportClient::SetupInitialLocalPlayerHK(UGameViewportClient* This, FString& OutError)
{
    return This->SetupInitialLocalPlayer(OutError);
}

// A packed build from 3.5 (CL-4000805) on enables the detours only after the engine has ticked past the packer's
// recheck (Packed::WaitForCodeCheck), by which time the engine has already set up the viewport's local player, so the
// detour above never runs and there is no console. Gives that viewport the console SetupInitialLocalPlayer would have.
void UGameViewportClient::CreateConsoleIfMissing()
{
#if ALLOW_CONSOLE
    UGameViewportClient* Viewport = GEngine ? GEngine->GameViewport : nullptr;
    if (!Viewport || Viewport->ViewportConsole || !GEngine->ConsoleClass)
    {
        return;
    }
    Viewport->ViewportConsole = NewObject<UConsole>(Viewport, GEngine->ConsoleClass);
    GLog->AddOutputDevice(Viewport->ViewportConsole);
    Log::Info(L"Console: the viewport was set up before the detours, so it gets its console now");
#endif
}

void UGameViewportClient::Hook()
{
    Hooks::Detour(L"UGameViewportClient::SetupInitialLocalPlayer", reinterpret_cast<void*>(Natives::UGameViewportClient_SetupInitialLocalPlayer), &SetupInitialLocalPlayerHK, &SetupInitialLocalPlayerOG);
}
