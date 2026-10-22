#include "pch.h"
#include "Heroes/FortHero.h"
#include "Heroes/FortHeroType.h"
#include "ReflectionParams.h"
#include "UObject/Class.h"

UFortHeroType* UFortHero::GetHeroTypeBP() const
{
    Reflection::FParams Params(StaticFunction_GetHeroTypeBP());
    return Params.Call(const_cast<UFortHero*>(this)) ? Params.Take<UFortHeroType*>(L"ReturnValue") : nullptr;
}
