#pragma once

#include "GameFramework/Controller.h"
#include "InputCoreTypes.h"

class APlayerCameraManager;
class UPlayer;

class APlayerController : public AController
{
    DefineUnrealClass(APlayerController, Engine)

public:
    DefineUProperty(UPlayer*, Player)
    DefineUProperty(APawn*, AcknowledgedPawn)
    DefineUProperty(APlayerCameraManager*, PlayerCameraManager)
    DefineUProperty(UObject*, CheatManager)

    void ConsoleKey(FKey Key);
    void SendToConsole(const FString& Command);
    void SendClientAdjustment();
    void AddCheats(bool bForce);

    static void Hook();

private:
    static void ConsoleKeyHK(APlayerController* This, FKey Key);
    static void SendToConsoleHK(APlayerController* This, const FString& Command);
};
