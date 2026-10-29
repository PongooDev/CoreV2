#include "pch.h"
#include "GameFramework/PlayerState.h"
#include "ReflectionParams.h"

FString APlayerState::GetPlayerName() const
{
    UFunction* Function = FindStaticFunction_GetPlayerName();
    if (!Function)
    {
        return PlayerName;
    }
    Reflection::FParams Params(Function, true);
    return Params.Call(const_cast<APlayerState*>(this)) ? Params.Take<FString>(L"ReturnValue") : FString();
}
