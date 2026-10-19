#include "pch.h"
#include "GameFramework/PlayerController.h"
#include "Engine/Console.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Finder.h"
#include "Hooks.h"
#include "Natives.h"
#include "Templates/Casts.h"
#include "UObject/UObjectIterator.h"

void APlayerController::ConsoleKey(FKey Key)
{
#if ALLOW_CONSOLE
    if (ULocalPlayer* LocalPlayer = Cast<ULocalPlayer>(Player))
    {
        if (LocalPlayer->ViewportClient && LocalPlayer->ViewportClient->ViewportConsole)
        {
            LocalPlayer->ViewportClient->ViewportConsole->InputKey(0, Key, IE_Pressed);
        }
    }
#endif
}

void APlayerController::SendToConsole(const FString& Command)
{
#if ALLOW_CONSOLE
    if (ULocalPlayer* LocalPlayer = Cast<ULocalPlayer>(Player))
    {
        if (LocalPlayer->ViewportClient && LocalPlayer->ViewportClient->ViewportConsole)
        {
            LocalPlayer->ViewportClient->ViewportConsole->ConsoleCommand(Command);
        }
    }
#endif
}

void APlayerController::ConsoleKeyHK(APlayerController* This, FKey Key)
{
    This->ConsoleKey(Key);
}

void APlayerController::SendToConsoleHK(APlayerController* This, const FString& Command)
{
    This->SendToConsole(Command);
}

void APlayerController::Hook()
{
    if (!Finder::ResolvePlayerControllerConsole())
    {
        return;
    }

    const UObject* Default = StaticClass()->GetDefaultObject();
    void* const* BaseVTable = Default ? *reinterpret_cast<void* const* const*>(Default) : nullptr;
    if (!BaseVTable)
    {
        return;
    }

    const int32_t ConsoleKeyIndex = Natives::APlayerController_ConsoleKeyIndex;
    const int32_t SendToConsoleIndex = Natives::APlayerController_SendToConsoleIndex;
    void* const StrippedConsoleKey = BaseVTable[ConsoleKeyIndex];
    void* const StrippedSendToConsole = BaseVTable[SendToConsoleIndex];

    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (!VTable)
        {
            continue;
        }
        if (VTable[ConsoleKeyIndex] == StrippedConsoleKey)
        {
            Hooks::SwapVTable(L"APlayerController::ConsoleKey", VTable, ConsoleKeyIndex, reinterpret_cast<void*>(&ConsoleKeyHK));
        }
        if (VTable[SendToConsoleIndex] == StrippedSendToConsole)
        {
            Hooks::SwapVTable(L"APlayerController::SendToConsole", VTable, SendToConsoleIndex, reinterpret_cast<void*>(&SendToConsoleHK));
        }
    }
}
