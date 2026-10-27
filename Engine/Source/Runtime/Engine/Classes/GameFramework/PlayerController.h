#pragma once

#include "GameFramework/Controller.h"
#include "InputCoreTypes.h"
#include "UObject/Stack.h"

#include <utility>
#include <vector>

class APlayerCameraManager;
class APlayerState;
class UPlayer;

class APlayerController : public AController
{
    DefineUnrealClass(APlayerController, Engine)

public:
    DefineUProperty(UPlayer*, Player)
    DefineUProperty(APawn*, AcknowledgedPawn)
    DefineUProperty(APlayerCameraManager*, PlayerCameraManager)
    DefineUProperty(UObject*, CheatManager)
    DefineUFunction(ToggleSpeaking)
    DefineUFunction(ClientEnableNetworkVoice)
    DefineUFunction(ClientMessage)
    DefineUFunction(ClientTeamMessage)

    void ConsoleKey(FKey Key);
    void SendToConsole(const FString& Command);
    void SendClientAdjustment();
    void AddCheats(bool bForce);
    // Engine voice chat: starts or stops sending this player's microphone through the game server.
    void ToggleSpeaking(bool bInSpeaking);
    FString ConsoleCommand(const FString& Command, bool bWriteToLog);
    void ClientMessage(const FString& S);

    static void Hook();
    // Voice is push to talk on desktop: the server's ClientEnableNetworkVoice(true) (open microphone, from its
    // bRequiresPushToTalk=false, which Epic set for consoles) no longer turns the microphone on.
    static void HookVoice();

private:
    static void ConsoleKeyHK(APlayerController* This, FKey Key);
    static void SendToConsoleHK(APlayerController* This, const FString& Command);
    DECLARE_FUNCTION(execClientEnableNetworkVoice);
    static void ClientTeamMessage_ImplementationHK(APlayerController* This, APlayerState* SenderPlayerState, const FString& S, FName Type, float MsgLifeTime);

    // Each controller vtable's own ClientTeamMessage_Implementation, as it was before the swap, and the engine's.
    static inline std::vector<std::pair<void* const*, void*>> ClientTeamMessageOriginals;
    static inline void* EngineClientTeamMessage = nullptr;
};
