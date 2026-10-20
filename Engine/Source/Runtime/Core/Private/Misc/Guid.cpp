#include "pch.h"
#include "Misc/Guid.h"

#include <combaseapi.h>

FGuid FGuid::NewGuid()
{
    FGuid Result;
    CoCreateGuid(reinterpret_cast<GUID*>(&Result));
    return Result;
}
