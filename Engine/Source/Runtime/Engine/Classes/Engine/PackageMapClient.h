#pragma once

#include "Containers/UnrealString.h"
#include "Misc/NetworkGuid.h"
#include "UObject/Object.h"

class FOutBunch;

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
