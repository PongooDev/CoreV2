#include "pch.h"
#include "Features/IModularFeatures.h"
#include "Natives.h"

IModularFeatures& IModularFeatures::Get()
{
    // The engine's singleton (FModularFeatures, a function-local static of its own IModularFeatures::Get).
    return Natives::IModularFeatures_Get();
}
