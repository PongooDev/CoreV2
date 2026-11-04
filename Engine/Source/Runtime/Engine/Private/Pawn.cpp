#include "pch.h"
#include "GameFramework/Pawn.h"
#include "ReflectionParams.h"

void APawn::OnRep_PlayerState()
{
    Reflection::FParams Params(StaticFunction_OnRep_PlayerState());
    Params.Call(this);
}

void APawn::AddMovementInput(const FVector& WorldDirection, float ScaleValue, bool bForce)
{
    Reflection::FParams Params(StaticFunction_AddMovementInput(), true);
    Params.Set(L"WorldDirection", WorldDirection);
    Params.Set(L"ScaleValue", ScaleValue);
    Params.Set(L"bForce", bForce);
    Params.Call(this);
}
