#include "pch.h"
#include "GameFramework/PlayerController.h"
#include "Engine/Console.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Finder.h"
#include "Hooks.h"
#include "Log.h"
#include "Natives.h"
#include "ReflectionParams.h"
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

void APlayerController::SendClientAdjustment()
{
    CallVirtual<void>(this, Natives::APlayerController_SendClientAdjustmentIndex);
}

void APlayerController::AddCheats(bool bForce)
{
    if (Natives::APlayerController_AddCheatsIndex >= 0 || Finder::ResolvePlayerControllerCheats())
    {
        CallVirtual<void>(this, Natives::APlayerController_AddCheatsIndex, bForce);
    }
}

FString APlayerController::ConsoleCommand(const FString& Command, bool bWriteToLog)
{
    // Returns what the command printed when bWriteToLog is false; with true the engine sends it to the log and returns "".
    FString Result;
    if (Natives::APlayerController_ConsoleCommandIndex >= 0)
    {
        CallVirtual<FString*>(this, Natives::APlayerController_ConsoleCommandIndex, &Result, &Command, bWriteToLog);
    }
    return Result;
}

void APlayerController::ClientMessage(const FString& S)
{
    Reflection::FParams Params(StaticFunction_ClientMessage());
    Params.Set(L"S", S);
    Params.Call(this);
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

void APlayerController::ToggleSpeaking(bool bInSpeaking)
{
    Reflection::FParams Params(StaticFunction_ToggleSpeaking());
    Params.Set(L"bInSpeaking", bInSpeaking);
    Params.Call(this);
}

void APlayerController::HookVoice()
{
    UFunction* Function = StaticFunction_ClientEnableNetworkVoice();
    if (Function && (Natives::FFrame_Step || Finder::ResolveScriptFrame()))
    {
        Function->SetNativeFunc(&execClientEnableNetworkVoice);
        Log::Info(L"Hook APlayerController::ClientEnableNetworkVoice: native (push to talk)");
    }
}

DEFINE_FUNCTION(APlayerController::execClientEnableNetworkVoice)
{
    P_GET_UBOOL(Z_Param_bEnable);
    P_FINISH;
    if (Z_Param_bEnable)
    {
        Log::Info(L"Voice chat: the server asked for an open microphone; Core keeps push to talk");
    }
}

void APlayerController::ConsoleKeyHK(APlayerController* This, FKey Key)
{
    This->ConsoleKey(Key);
}

void APlayerController::SendToConsoleHK(APlayerController* This, const FString& Command)
{
    This->SendToConsole(Command);
}

void APlayerController::ClientTeamMessage_ImplementationHK(APlayerController* This, APlayerState* SenderPlayerState, const FString& S, FName Type, float MsgLifeTime)
{
    using FClientTeamMessage = void (*)(APlayerController*, APlayerState*, const FString&, FName, float);
    void* const* const VTable = *reinterpret_cast<void* const* const*>(This);
    for (const auto& [ControllerVTable, Original] : ClientTeamMessageOriginals)
    {
        if (ControllerVTable == VTable)
        {
            reinterpret_cast<FClientTeamMessage>(Original)(This, SenderPlayerState, S, Type, MsgLifeTime);
            break;
        }
    }

    // AFortPlayerController's override (Cert 0x743050, 1.2 0x140739E20, 1.7.2 0x7FA570) only hands the message to the
    // HUD's notifications, so cheat replies ("God mode on", the server's "Command not recognized") never reached the
    // console (#75). The engine's own body writes it to the viewport console.
    if (EngineClientTeamMessage)
    {
        reinterpret_cast<FClientTeamMessage>(EngineClientTeamMessage)(This, SenderPlayerState, S, Type, MsgLifeTime);
    }
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
    const int32_t ClientTeamMessageIndex = Finder::ImplementationIndex(StaticFunction_ClientTeamMessage());
    EngineClientTeamMessage = ClientTeamMessageIndex >= 0 ? BaseVTable[ClientTeamMessageIndex] : nullptr;
    if (!EngineClientTeamMessage)
    {
        Log::Error(L"Finder: APlayerController::ClientTeamMessage_Implementation slot not found");
    }

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

        void* Original = nullptr;
        if (EngineClientTeamMessage && VTable[ClientTeamMessageIndex] != EngineClientTeamMessage && VTable[ClientTeamMessageIndex] != reinterpret_cast<void*>(&ClientTeamMessage_ImplementationHK)
            && Hooks::SwapVTable(L"APlayerController::ClientTeamMessage_Implementation", VTable, ClientTeamMessageIndex, reinterpret_cast<void*>(&ClientTeamMessage_ImplementationHK), &Original) && Original)
        {
            ClientTeamMessageOriginals.emplace_back(VTable, Original);
        }
    }
}
