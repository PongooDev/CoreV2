#include "pch.h"
#include "Engine/GameViewportClient.h"
#include "CoreGlobals.h"
#include "Engine/Engine.h"
#include "Hooks.h"
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

void UGameViewportClient::Hook()
{
    Hooks::Detour(L"UGameViewportClient::SetupInitialLocalPlayer", reinterpret_cast<void*>(Natives::UGameViewportClient_SetupInitialLocalPlayer), &SetupInitialLocalPlayerHK, &SetupInitialLocalPlayerOG);
}
