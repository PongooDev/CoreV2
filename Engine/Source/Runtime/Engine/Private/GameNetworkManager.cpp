#include "pch.h"
#include "GameFramework/GameNetworkManager.h"
#include "Natives.h"

bool AGameNetworkManager::IsInLowBandwidthMode()
{
    return CallVirtual<bool>(this, Natives::AGameNetworkManager_IsInLowBandwidthModeIndex);
}
