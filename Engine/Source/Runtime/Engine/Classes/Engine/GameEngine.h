#pragma once

#include "Engine/Engine.h"

class UGameInstance;

class UGameEngine : public UEngine
{
    DefineUnrealClass(UGameEngine, Engine)
    DefineUProperty(UGameInstance*, GameInstance)

public:
    static void Hook();
    static void SetServerTickRate();
    // Lets go of a game window the PreLoadScreen handed the engine (4.5+) on a server, which makes no viewport for it.
    static void ReleasePreLoadScreenWindow();

private:
    static inline void (*TickOG)(UGameEngine* This, float DeltaSeconds, bool bIdleMode) = nullptr;
    static void TickHK(UGameEngine* This, float DeltaSeconds, bool bIdleMode);
};
