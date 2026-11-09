#pragma once

#include "UObject/Object.h"

// The player's own settings (the cloud-saved ClientSettings.Sav), on the client.
class UFortClientSettingsRecord : public UObject
{
    DefineUnrealClass(UFortClientSettingsRecord, FortniteGame)

public:
    // Settings > Audio > Voice Chat, and its Push To Talk switch (off: open microphone). The game's push to talk
    // handlers (4.5 0xFA4BB0 pressed, 0xFA50E0 released) do nothing unless both are on. Reflected from 3.0 at least.
    DefineUProperty(bool, bEnableVoiceChat)
    DefineUProperty(bool, bEnableVoiceChatPTT)
    static bool HasVoiceChatSettings() { return Member_bEnableVoiceChat.IsAvailable() && Member_bEnableVoiceChatPTT.IsAvailable(); }
};
