#pragma once

#include "GameFramework/Controller.h"
#include "InputCoreTypes.h"

class UPlayer;

class APlayerController : public AController
{
    DefineUnrealClass(APlayerController, Engine)

public:
    DefineUProperty(UPlayer*, Player)

    void ConsoleKey(FKey Key);
    void SendToConsole(const FString& Command);

    static void Hook();

private:
    static void ConsoleKeyHK(APlayerController* This, FKey Key);
    static void SendToConsoleHK(APlayerController* This, const FString& Command);
};
