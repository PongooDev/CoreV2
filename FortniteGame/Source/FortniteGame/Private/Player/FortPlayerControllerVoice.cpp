#include "pch.h"
#include "Player/FortPlayerController.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "Finder.h"
#include "Hooks.h"
#include "Log.h"
#include "Natives.h"
#include "OnlineVoiceImpl.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

// Voice chat (Core-Backend #54). 1.7.2's own voice chat is Vivox, a third-party service; its push to talk handlers
// (AFortPlayerController's, and AFortPlayerControllerAthena's overrides 0x438810 pressed, 0x4389F0 released) do
// nothing without the VoiceChat modular feature. The engine's voice chat
// (FOnlineVoiceImpl, UVoiceChannel, Opus) is intact and sends voice through the game server, which forwards it to the
// other players: consoles used it. FConfigCacheIni::EnableVoice turns it on; here the key starts and stops it.

void AFortPlayerController::HookVoice()
{
    APlayerController::HookVoice();
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
    This->ToggleSpeaking(true);
}

void AFortPlayerController::PushToTalkReleasedHK(AFortPlayerController* This)
{
    Log::Info(L"Voice chat: stopped talking");
    This->ToggleSpeaking(false);
}
