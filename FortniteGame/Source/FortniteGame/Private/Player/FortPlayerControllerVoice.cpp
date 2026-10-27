#include "pch.h"
#include "Player/FortPlayerController.h"
#include "Finder.h"
#include "Hooks.h"
#include "Log.h"
#include "Natives.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

// Voice chat (Core-Backend #54). 1.7.2's own voice chat is Vivox, a third-party service; its push to talk handlers
// (0x438810 pressed, 0x4389F0 released) do nothing without the VoiceChat modular feature. The engine's voice chat
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

    const int32_t PressedIndex = Natives::AFortPlayerController_PushToTalkPressedIndex;
    const int32_t ReleasedIndex = Natives::AFortPlayerController_PushToTalkReleasedIndex;
    void* const Pressed = BaseVTable[PressedIndex];
    void* const Released = BaseVTable[ReleasedIndex];
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (!VTable)
        {
            continue;
        }
        if (VTable[PressedIndex] == Pressed)
        {
            Hooks::SwapVTable(L"AFortPlayerController::PushToTalk pressed", VTable, PressedIndex, reinterpret_cast<void*>(&PushToTalkPressedHK));
        }
        if (VTable[ReleasedIndex] == Released)
        {
            Hooks::SwapVTable(L"AFortPlayerController::PushToTalk released", VTable, ReleasedIndex, reinterpret_cast<void*>(&PushToTalkReleasedHK));
        }
    }
}

void AFortPlayerController::PushToTalkPressedHK(AFortPlayerController* This)
{
    Log::Info(L"Voice chat: talking");
    This->ToggleSpeaking(true);
}

void AFortPlayerController::PushToTalkReleasedHK(AFortPlayerController* This)
{
    Log::Info(L"Voice chat: stopped talking");
    This->ToggleSpeaking(false);
}
