#pragma once

#include "GameFramework/Controller.h"
#include "InputCoreTypes.h"
#include "UObject/Stack.h"

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
    DefineUProperty(UClass*, CheatClass)
    DefineUFunction(ToggleSpeaking)
    DefineUFunction(ClientEnableNetworkVoice)
    DefineUFunction(ClientMessage)

    void ConsoleKey(FKey Key);
    void SendToConsole(const FString& Command);
    void SendClientAdjustment();
    void AddCheats(bool bForce);
    // Engine voice chat: starts or stops sending this player's microphone through the game server.
    void ToggleSpeaking(bool bInSpeaking);
    FString ConsoleCommand(const FString& Command, bool bWriteToLog);
    void ClientMessage(const FString& S);

    static void Hook();
    // Client: every local player controller gets its cheat manager as soon as it exists (#75), the way the controller's
    // CheatClass would on a listen server, so the game's own cheats work from the console after every travel.
    static void EnsureLocalCheatManager(class UGameInstance* GameInstance);
    // Voice is push to talk on desktop: the server's ClientEnableNetworkVoice(true) (open microphone, from its
    // bRequiresPushToTalk=false, which Epic set for consoles) no longer turns the microphone on.
    static void HookVoice();

private:
    static void ConsoleKeyHK(APlayerController* This, FKey Key);
    static void SendToConsoleHK(APlayerController* This, const FString& Command);
    DECLARE_FUNCTION(execClientEnableNetworkVoice);
};
