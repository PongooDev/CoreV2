#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"

struct FMcpServicePermissions
{
    FString Name;
    FString Id;
    FString Key;
};

static_assert(sizeof(FMcpServicePermissions) == 0x30);
