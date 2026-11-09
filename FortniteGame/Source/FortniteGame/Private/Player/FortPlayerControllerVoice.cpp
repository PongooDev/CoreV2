#include "pch.h"
#include "Player/FortPlayerController.h"
#include "Player/FortClientSettingsRecord.h"
#include "Player/FortLocalPlayer.h"
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

#include <algorithm>
#include <unordered_set>
#include <vector>

// Voice chat (Core-Backend #54). 1.7.2's own voice chat is Vivox, a third-party service; its push to talk handlers
// (AFortPlayerController's, and AFortPlayerControllerAthena's overrides 0x438810 pressed, 0x4389F0 released) do
// nothing without the VoiceChat modular feature. The engine's voice chat
// (FOnlineVoiceImpl, UVoiceChannel, Opus) is intact and sends voice through the game server, which forwards it to the
// other players: consoles used it. FConfigCacheIni::EnableVoice turns it on; here the key starts and stops it.

void AFortPlayerController::HookVoice()
{
    APlayerController::HookVoice();
    Finder::ResolveVoiceHandshake();
    FOnlineVoiceImpl::OnTick = &TickVoice;
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

    // The bindings' virtuals can be the Battle Royale controller's own (1.8.2 0x32F/0x330, 1.9 0x335/0x336): the base
    // controller's vtable (1.9 0x31A slots) and the other modes' end before them, and their entries there are the next
    // class's vtable. Swapping those swapped other classes' functions (1.9: UFortLocalPlayer's and AFortPlayerState's
    // UObject slots 0x1B/0x1C). Without RTTI the classes' vtables sit back to back, so a vtable holds a slot when no
    // other class's vtable starts before it.
    std::vector<uintptr_t> VTableStarts;
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        const UObject* ClassDefault = Class ? Class->GetDefaultObject(false) : nullptr;
        if (ClassDefault)
        {
            VTableStarts.push_back(*reinterpret_cast<const uintptr_t*>(ClassDefault));
        }
    }
    std::sort(VTableStarts.begin(), VTableStarts.end());
    const auto HasSlot = [&VTableStarts](void* const* VTable, int32_t Index)
    {
        const uintptr_t Start = reinterpret_cast<uintptr_t>(VTable);
        const auto Next = std::upper_bound(VTableStarts.begin(), VTableStarts.end(), Start);
        return VTable && (Next == VTableStarts.end() || *Next > Start + Index * sizeof(void*));
    };

    UClass* const AthenaClass = AFortPlayerControllerAthena::StaticClass();
    const UObject* const AthenaDefault = AthenaClass ? AthenaClass->GetDefaultObject() : nullptr;
    void* const* const AthenaVTable = AthenaDefault ? *reinterpret_cast<void* const* const*>(AthenaDefault) : nullptr;
    void* const Pressed[] = { HasSlot(BaseVTable, PressedIndex) ? BaseVTable[PressedIndex] : nullptr, HasSlot(AthenaVTable, PressedIndex) ? AthenaVTable[PressedIndex] : nullptr };
    void* const Released[] = { HasSlot(BaseVTable, ReleasedIndex) ? BaseVTable[ReleasedIndex] : nullptr, HasSlot(AthenaVTable, ReleasedIndex) ? AthenaVTable[ReleasedIndex] : nullptr };
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
        if (HasSlot(VTable, PressedIndex) && IsOneOf(VTable[PressedIndex], Pressed))
        {
            Hooks::SwapVTable(L"AFortPlayerController::PushToTalk pressed", VTable, PressedIndex, reinterpret_cast<void*>(&PushToTalkPressedHK));
        }
        if (HasSlot(VTable, ReleasedIndex) && IsOneOf(VTable[ReleasedIndex], Released))
        {
            Hooks::SwapVTable(L"AFortPlayerController::PushToTalk released", VTable, ReleasedIndex, reinterpret_cast<void*>(&PushToTalkReleasedHK));
        }
    }
}

namespace
{
    ULocalPlayer* GetFirstLocalPlayer()
    {
        UGameEngine* const Engine = Natives::GEngine ? Cast<UGameEngine>(static_cast<UEngine*>(GEngine)) : nullptr;
        UGameInstance* const GameInstance = Engine ? static_cast<UGameInstance*>(Engine->GameInstance) : nullptr;
        const TArray<ULocalPlayer*>* const LocalPlayers = GameInstance ? &static_cast<TArray<ULocalPlayer*>&>(GameInstance->LocalPlayers) : nullptr;
        return LocalPlayers && LocalPlayers->Num() > 0 ? (*LocalPlayers)[0] : nullptr;
    }

    // The player's own voice chat settings, where the build reflects them (3.0 on): null on older builds and until the
    // settings are read, and Core then keeps push to talk as before.
    const UFortClientSettingsRecord* GetVoiceChatSettings(UPlayer* Player)
    {
        if (!Player || !UFortLocalPlayer::HasClientSettingsRecord() || !UFortClientSettingsRecord::HasVoiceChatSettings())
        {
            return nullptr;
        }
        const UFortLocalPlayer* const FortPlayer = Cast<UFortLocalPlayer>(Player);
        return FortPlayer ? static_cast<const UFortClientSettingsRecord*>(FortPlayer->ClientSettingsRecord) : nullptr;
    }

    // Whether the player talks with the push to talk key: the game's own handlers (4.5 0xFA4BB0, 0xFA50E0) act only with
    // voice chat and push to talk both on.
    bool UsesPushToTalk(const UFortClientSettingsRecord* Settings)
    {
        return !Settings || (Settings->bEnableVoiceChat && Settings->bEnableVoiceChatPTT);
    }

    // Whether the player's setting is an open microphone: voice chat on, push to talk off.
    bool UsesOpenMicrophone(const UFortClientSettingsRecord* Settings)
    {
        return Settings && Settings->bEnableVoiceChat && !Settings->bEnableVoiceChatPTT;
    }

    // The controller Core opened the microphone for (open microphone setting); compared only.
    const AFortPlayerController* OpenMicrophoneController = nullptr;

    void StartSpeaking(AFortPlayerController* Controller)
    {
        // The engine's FOnlineVoiceImpl::RegisterLocalTalker (4.5 0x3A9870) calls its own StartNetworkedVoice (slot 3)
        // before the voice engine's RegisterLocalTalker gives the device to the player, so a session's registration logs
        // "Device is currently owned by another user" and leaves the microphone off: the next StartNetworkedVoice, from
        // ToggleSpeaking, starts it (#182).
        FOnlineVoiceImpl::RegisterLocalTalker(0);

        // The client's voice channel sends the microphone's packets only once the controller's voice handshake is
        // complete (UVoiceChannel's tick, 1.7.2 0x24A74D0, checks the connection's controller's
        // MuteList.bHasVoiceHandshakeCompleted), which the server's ClientVoiceHandshakeComplete sets. Without it every
        // packet stayed on the client (Core-Backend #54: packets recorded, none received by the server). The server
        // forwards voice only to players whose handshake it completed itself, so completing it here only lets this
        // player's voice leave.
        if (Finder::ResolveVoiceHandshake())
        {
            uint8_t& bHandshakeCompleted = *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(Controller) + Natives::APlayerController_VoiceHandshakeCompletedOffset);
            if (!bHandshakeCompleted)
            {
                Log::Info(L"Voice chat: the server's voice handshake hasn't arrived; Core completes it so the voice is sent");
                bHandshakeCompleted = 1;
            }
        }
        Controller->ToggleSpeaking(true);
    }
}

void AFortPlayerController::PushToTalkPressedHK(AFortPlayerController* This)
{
    // With an open microphone (or voice chat off) the key does nothing, as in the game's own handler; letting go of it
    // would close the open microphone.
    if (!UsesPushToTalk(GetVoiceChatSettings(This->Player)))
    {
        return;
    }
    Log::Info(L"Voice chat: talking");
    StartSpeaking(This);
}

void AFortPlayerController::PushToTalkReleasedHK(AFortPlayerController* This)
{
    if (!UsesPushToTalk(GetVoiceChatSettings(This->Player)))
    {
        return;
    }
    Log::Info(L"Voice chat: stopped talking");
    This->ToggleSpeaking(false);
}

void AFortPlayerController::UpdateOpenMicrophone()
{
    // 3.0 and later have a Push To Talk switch in the audio settings. With it off (an open microphone, which Vivox's
    // TransmitToAllChannels honoured) the player never presses the key, and Core, which only talked on the key, never
    // started the microphone: the session's registration leaves it off (StartSpeaking), so the voice interface was asked
    // for packets but recorded none (#182). The server's ClientEnableNetworkVoice(true), Epic's ToggleSpeaking(true),
    // opens it for such a player. The setting is read every frame, so switching it during a match applies.
    UPlayer* const Player = GetFirstLocalPlayer();
    AFortPlayerController* const Controller = Player ? Cast<AFortPlayerController>(static_cast<APlayerController*>(Player->PlayerController)) : nullptr;
    if (OpenMicrophoneController && OpenMicrophoneController != Controller)
    {
        // The controller Core opened it for is gone (travel): the voice interface keeps the microphone on until told.
        if (Controller)
        {
            Controller->ToggleSpeaking(false);
        }
        OpenMicrophoneController = nullptr;
    }
    if (!Controller)
    {
        return;
    }

    const bool bOpen = OpenMicrophoneController == Controller;
    const bool bWantOpen = APlayerController::NetworkVoiceController == Controller && UsesOpenMicrophone(GetVoiceChatSettings(Player));
    if (bWantOpen && !bOpen)
    {
        Log::Info(L"Voice chat: open microphone (the player's voice chat setting has push to talk off)");
        OpenMicrophoneController = Controller;
        StartSpeaking(Controller);
    }
    else if (!bWantOpen && bOpen)
    {
        Log::Info(L"Voice chat: open microphone closed (push to talk or voice chat switched off, or the server asked for push to talk)");
        OpenMicrophoneController = nullptr;
        Controller->ToggleSpeaking(false);
    }
}

void AFortPlayerController::TickVoice()
{
    UpdateOpenMicrophone();
    UpdateTeamTalking();
}

void AFortPlayerController::UpdateTeamTalking()
{
    // The team list's speaker icons (AthenaTeamMemberInfo's TalkingBrush) follow the team's player states' bIsTalking:
    // the controller's tick (1.7.2 0x417630) gathers it for TeamMembers and, when it changes, the HUD context
    // (0x2D55BD0) broadcasts OnTeamMemberTalkingChanged to AthenaTeamInfo's HandleTeamTalkingChanged. Only Vivox's
    // participant updates set it, so with the engine's voice chat every icon stayed idle (Core-Backend #54). 1.11's
    // team list follows SquadMembers instead (the tick's gather, 0x42C968, and GetSquadTalking, 0x43BC20, read
    // bIsTalking from it; HandleSquadTalkingChanged), so there the squad's states are set (#182).
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
    const TArray<AFortPlayerStateAthena*>& TeamMembers = AFortPlayerControllerAthena::HasSquadMembers() ? Controller->GetMember_SquadMembers() : Controller->GetMember_TeamMembers();
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
                Log::Info(L"Voice chat: the team list shows {} ({}) talking", Member->GetPlayerName().ToStdString(), Member->UniqueId.ToString().ToStdString());
            }
        }
    }
}
