#pragma once

#include "Engine/Player.h"

class UGameViewportClient;

class ULocalPlayer : public UPlayer
{
    DefineUnrealClass(ULocalPlayer, Engine)

public:
    DefineUProperty(UGameViewportClient*, ViewportClient)
};
