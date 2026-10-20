#include "pch.h"
#include "Pawns/FortPawn.h"
#include "ReflectionParams.h"
#include "UObject/Class.h"

bool AFortPawn::IsDead() const
{
    Reflection::FParams Params(StaticFunction_IsDead());
    return Params.Call(const_cast<AFortPawn*>(this)) && Params.Take<bool>(L"ReturnValue");
}
