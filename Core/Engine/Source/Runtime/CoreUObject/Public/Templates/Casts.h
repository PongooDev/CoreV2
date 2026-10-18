#pragma once

#include "UObject/Class.h"

template <class To, class From>
To* Cast(From* Src)
{
    return Src && Src->IsA(To::StaticClass()) ? static_cast<To*>(Src) : nullptr;
}

template <class To, class From>
const To* Cast(const From* Src)
{
    return Src && Src->IsA(To::StaticClass()) ? static_cast<const To*>(Src) : nullptr;
}
