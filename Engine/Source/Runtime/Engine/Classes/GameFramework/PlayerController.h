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
    DefineUFunction(ServerAcknowledgePossession)
    DefineUFunction(ClientSetViewTarget)
    // The engine's "waiting to respawn": ServerRestartPlayer restarts a Spectating controller only with it.
    DefineBitfieldUProperty(bPlayerIsWaiting)
    static bool HasPlayerIsWaiting() { return Member_bPlayerIsWaiting.IsAvailable(); }

    void ConsoleKey(FKey Key);
    void SendToConsole(const FString& Command);
    void SendClientAdjustment();
    void AddCheats(bool bForce);
    // Engine voice chat: starts or stops sending this player's microphone through the game server.
    void ToggleSpeaking(bool bInSpeaking);
    FString ConsoleCommand(const FString& Command, bool bWriteToLog);
    void ClientMessage(const FString& S);
    // Server: the client's camera views A at once (the engine's ClientSetViewTarget RPC, default transition: no blend).
    // The server's camera manager sends it only when its own view target changes, so a view the client set on its own
    // (a camera actor) is only left through this. False where the build has no such function.
    bool ClientSetViewTarget(AActor* A);

    static void Hook();
    // Client: every local player controller gets its cheat manager as soon as it exists (#75), the way the controller's
    // CheatClass would on a listen server, so the game's own cheats work from the console after every travel.
    static void EnsureLocalCheatManager(class UGameInstance* GameInstance);
    // Voice is push to talk on desktop: the server's ClientEnableNetworkVoice(true) (open microphone, from its
    // bRequiresPushToTalk=false, which Epic set for consoles) no longer turns the microphone on by itself. It is kept
    // here instead, and the game opens the microphone when the player's own voice chat setting is open microphone.
    static void HookVoice();
    // Client: the controller whose server last asked for an open microphone (null once it asks for push to talk). Only
    // compared, never dereferenced: it may be gone after a travel.
    static inline const APlayerController* NetworkVoiceController = nullptr;

private:
    static void ConsoleKeyHK(APlayerController* This, FKey Key);
    static void SendToConsoleHK(APlayerController* This, const FString& Command);
    DECLARE_FUNCTION(execClientEnableNetworkVoice);
};
