#pragma once

#include "Containers/UnrealString.h"
#include "Logging/LogMacros.h"
#include "Misc/NetworkGuid.h"
#include "UObject/Object.h"

class FOutBunch;

DECLARE_LOG_CATEGORY_EXTERN(LogNetPackageMap, Log, All);

class UPackageMap : public UObject
{
    DefineUnrealClass(UPackageMap, CoreUObject)

public:
    bool WriteObject(FOutBunch& Ar, UObject* InOuter, FNetworkGUID NetGUID, FString ObjName);
};

class FNetGUIDCache
{
public:
    bool SupportsObject(const UObject* Object) const;
};
