#include "pch.h"
#include "Engine/Engine.h"
#include "Hooks.h"
#include "Natives.h"

void FOnNetworkFailure::Broadcast(UWorld* World, UNetDriver* NetDriver, ENetworkFailure::Type FailureType, const FString& ErrorString) const
{
    Natives::FOnNetworkFailure_Broadcast(const_cast<FOnNetworkFailure*>(this), World, NetDriver, FailureType, ErrorString);
}

bool UEngine::CreateNamedNetDriver(UWorld* InWorld, FName NetDriverName, FName NetDriverDefinition)
{
    return Natives::UEngine_CreateNamedNetDriver(this, InWorld, NetDriverName, NetDriverDefinition);
}

UNetDriver* UEngine::FindNamedNetDriver(const UWorld* InWorld, FName NetDriverName)
{
    return Natives::UEngine_FindNamedNetDriver(this, InWorld, NetDriverName);
}

float UEngine::GetMaxTickRate(float DeltaTime, bool bAllowFrameRateSmoothing) const
{
    return CallVirtual<float>(this, Natives::UEngine_GetMaxTickRateIndex, DeltaTime, bAllowFrameRateSmoothing);
}

void UEngine::Hook()
{
    for (uint8_t* Check : Sites::EngineInitDedicatedServerChecks)
    {
        Hooks::SkipBranch(L"UEngine::Init IsRunningDedicatedServer", Check);
    }

    if (Sites::MaxTickRateWait.SpinLoop)
    {
        const uint8_t AlwaysSleep[] = { 0x90, 0x90 };
        const uint8_t FullWait[] = { 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 };
        const uint8_t NoSpin[] = { 0xEB };
        Hooks::WriteBytes(L"UEngine::UpdateTimeAndHandleMaxTickRate IsRunningDedicatedServer", Sites::MaxTickRateWait.SleepThreshold, AlwaysSleep);
        Hooks::WriteBytes(L"UEngine::UpdateTimeAndHandleMaxTickRate IsRunningDedicatedServer", Sites::MaxTickRateWait.SleepSlack, FullWait);
        Hooks::WriteBytes(L"UEngine::UpdateTimeAndHandleMaxTickRate IsRunningDedicatedServer", Sites::MaxTickRateWait.SpinLoop, NoSpin);
    }
}
