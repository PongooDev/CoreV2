#include "pch.h"
#include "Engine/PackageMapClient.h"
#include "Natives.h"
#include "Net/DataBunch.h"

#include <new>

DEFINE_LOG_CATEGORY(LogNetPackageMap);

bool UPackageMap::WriteObject(FOutBunch& Ar, UObject* InOuter, FNetworkGUID NetGUID, FString ObjName)
{
    alignas(FString) uint8_t CalleeOwnedName[sizeof(FString)];
    FString* PassedName = new (CalleeOwnedName) FString(ObjName);
    return CallVirtual<bool>(this, Natives::UPackageMap_WriteObjectIndex, &Ar, InOuter, NetGUID, PassedName);
}

bool FNetGUIDCache::SupportsObject(const UObject* Object) const
{
    return Natives::FNetGUIDCache_SupportsObject(this, Object);
}
