#include "pch.h"
#include "GameFramework/CheatManager.h"
#include "GameFramework/PlayerController.h"
#include "Templates/Casts.h"

APlayerController* UCheatManager::GetOuterAPlayerController() const
{
    return Cast<APlayerController>(GetOuter());
}
