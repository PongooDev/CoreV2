#include "pch.h"
#include "Player/FortPlayerController.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "Athena/FortPlayerStateAthena.h"
#include "Engine/GameEngine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Finder.h"
#include "GameFramework/PlayerState.h"
#include "Hooks.h"
#include "Log.h"
#include "Natives.h"
#include "OnlineVoiceImpl.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

#include <unordered_set>

// Voice chat (Core-Backend #54). 1.7.2's own voice chat is Vivox, a third-party service; its push to talk handlers
// (AFortPlayerController's, and AFortPlayerControllerAthena's overrides 0x438810 pressed, 0x4389F0 released) do
// nothing without the VoiceChat modular feature. The engine's voice chat
// (FOnlineVoiceImpl, UVoiceChannel, Opus) is intact and sends voice through the game server, which forwards it to the
// other players: consoles used it. FConfigCacheIni::EnableVoice turns it on; here the key starts and stops it.

void AFortPlayerController::HookVoice()
{
    APlayerController::HookVoice();
    Finder::ResolveVoiceHandshake();
    FOnlineVoiceImpl::OnTick = &UpdateTeamTalking;
    if (!Finder::ResolvePushToTalk())
    {
        return;
    }

    const UObject* Default = StaticClass()->GetDefaultObject();
    void* const* BaseVTable = Default ? *reinterpret_cast<void* const* const*>(Default) : nullptr;
    if (!BaseVTable)
    {
        return;
    }

    // Battle Royale's controller overrides both (Vivox's handlers above), so its classes are matched on its own entries
    // too: matched on the base controller's alone, push to talk never reached voice chat in Battle Royale.
    const int32_t PressedIndex = Natives::AFortPlayerController_PushToTalkPressedIndex;
    const int32_t ReleasedIndex = Natives::AFortPlayerController_PushToTalkReleasedIndex;
    UClass* const AthenaClass = AFortPlayerControllerAthena::StaticClass();
    const UObject* const AthenaDefault = AthenaClass ? AthenaClass->GetDefaultObject() : nullptr;
    void* const* const AthenaVTable = AthenaDefault ? *reinterpret_cast<void* const* const*>(AthenaDefault) : nullptr;
    void* const Pressed[] = { BaseVTable[PressedIndex], AthenaVTable ? AthenaVTable[PressedIndex] : nullptr };
    void* const Released[] = { BaseVTable[ReleasedIndex], AthenaVTable ? AthenaVTable[ReleasedIndex] : nullptr };
    const auto IsOneOf = [](void* Entry, void* const (&Handlers)[2]) { return Entry && (Entry == Handlers[0] || Entry == Handlers[1]); };
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (!VTable)
        {
            continue;
        }
        if (IsOneOf(VTable[PressedIndex], Pressed))
        {
            Hooks::SwapVTable(L"AFortPlayerController::PushToTalk pressed", VTable, PressedIndex, reinterpret_cast<void*>(&PushToTalkPressedHK));
        }
        if (IsOneOf(VTable[ReleasedIndex], Released))
        {
            Hooks::SwapVTable(L"AFortPlayerController::PushToTalk released", VTable, ReleasedIndex, reinterpret_cast<void*>(&PushToTalkReleasedHK));
        }
    }
}

void AFortPlayerController::PushToTalkPressedHK(AFortPlayerController* This)
{
    Log::Info(L"Voice chat: talking");
    FOnlineVoiceImpl::RegisterLocalTalker(0);

    // The client's voice channel sends the microphone's packets only once the controller's voice handshake is complete
    // (UVoiceChannel's tick, 1.7.2 0x24A74D0, checks the connection's controller's MuteList.bHasVoiceHandshakeCompleted),
    // which the server's ClientVoiceHandshakeComplete sets. Without it every packet stayed on the client (Core-Backend #54:
    // packets recorded, none received by the server). The server forwards voice only to players whose handshake it
    // completed itself, so completing it here only lets this player's voice leave.
    if (Finder::ResolveVoiceHandshake())
    {
        uint8_t& bHandshakeCompleted = *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(This) + Natives::APlayerController_VoiceHandshakeCompletedOffset);
        if (!bHandshakeCompleted)
        {
            Log::Info(L"Voice chat: the server's voice handshake hasn't arrived; Core completes it so the voice is sent");
            bHandshakeCompleted = 1;
        }
    }
    This->ToggleSpeaking(true);
}

void AFortPlayerController::PushToTalkReleasedHK(AFortPlayerController* This)
{
    Log::Info(L"Voice chat: stopped talking");
    This->ToggleSpeaking(false);
}

void AFortPlayerController::UpdateTeamTalking()
{
    // The team list's speaker icons (AthenaTeamMemberInfo's TalkingBrush) follow the team's player states' bIsTalking:
    // the controller's tick (1.7.2 0x417630) gathers it for TeamMembers and, when it changes, the HUD context
    // (0x2D55BD0) broadcasts OnTeamMemberTalkingChanged to AthenaTeamInfo's HandleTeamTalkingChanged. Only Vivox's
    // participant updates set it, so with the engine's voice chat every icon stayed idle (Core-Backend #54).
    UClass* const AthenaClass = AFortPlayerControllerAthena::StaticClass();
    UGameEngine* const Engine = AthenaClass && Natives::GEngine ? Cast<UGameEngine>(static_cast<UEngine*>(GEngine)) : nullptr;
    UGameInstance* const GameInstance = Engine ? static_cast<UGameInstance*>(Engine->GameInstance) : nullptr;
    const TArray<ULocalPlayer*>* const LocalPlayers = GameInstance ? &static_cast<TArray<ULocalPlayer*>&>(GameInstance->LocalPlayers) : nullptr;
    ULocalPlayer* const Player = LocalPlayers && LocalPlayers->Num() > 0 ? (*LocalPlayers)[0] : nullptr;
    AFortPlayerControllerAthena* const Controller = Player ? Cast<AFortPlayerControllerAthena>(static_cast<APlayerController*>(Player->PlayerController)) : nullptr;
    if (!Controller)
    {
        return;
    }

    static std::unordered_set<const AFortPlayerStateAthena*> Shown;
    const APlayerState* const LocalState = Controller->PlayerState;
    const TArray<AFortPlayerStateAthena*>& TeamMembers = Controller->TeamMembers;
    for (int32_t Index = 0; Index < TeamMembers.Num(); ++Index)
    {
        AFortPlayerStateAthena* const Member = TeamMembers[Index];
        if (!Member)
        {
            continue;
        }
        // The server doesn't send the player's own voice back: theirs is the microphone recording.
        const bool bTalking = static_cast<const APlayerState*>(Member) == LocalState ? FOnlineVoiceImpl::IsLocalPlayerTalking() : FOnlineVoiceImpl::IsRemotePlayerTalking(Member->UniqueId);
        if (Member->bIsTalking != bTalking)
        {
            Member->bIsTalking = bTalking;
            if (bTalking && Shown.insert(Member).second)
            {
                Log::Info(L"Voice chat: the team list shows {} ({}) talking", Member->PlayerName.ToStdString(), Member->UniqueId.ToString().ToStdString());
            }
        }
    }
}
