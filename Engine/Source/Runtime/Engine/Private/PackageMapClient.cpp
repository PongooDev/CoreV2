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
    // The weak pointer is passed as null, as the engine's callers without one do: 1.8's SupportsObject reads it whenever it
    // isn't, and a call without it left whatever was in r8, which crashed a 1.8 server reading 0xF4CB at 0x22B20BC (#119).
    return Natives::FNetGUIDCache_SupportsObject(this, Object, nullptr);
}
