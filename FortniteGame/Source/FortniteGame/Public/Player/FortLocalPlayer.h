#pragma once

#include "Engine/LocalPlayer.h"

class UFortClientSettingsRecord;

class UFortLocalPlayer : public ULocalPlayer
{
    DefineUnrealClass(UFortLocalPlayer, FortniteGame)

public:
    // Null until the player's settings are read.
    DefineUProperty(UFortClientSettingsRecord*, ClientSettingsRecord)
    static bool HasClientSettingsRecord() { return Member_ClientSettingsRecord.IsAvailable(); }
};
