#include "pch.h"
#include "Weapons/FortProjectileAthena.h"
#include "ReflectionParams.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

void AFortProjectileAthena::ServerNotifyPawnHit(const void* Hit)
{
    Reflection::FParams Params(StaticFunction_ServerNotifyPawnHit(), true);
    if (Params.CopyStruct(L"Hit", Hit))
    {
        Params.Call(this);
    }
}

UScriptStruct* AFortProjectileAthena::HitResultStruct()
{
    UFunction* Function = FindStaticFunction_ServerNotifyPawnHit();
    UProperty* Hit = Function ? Function->FindPropertyByName(FName(L"Hit", FNAME_Find)) : nullptr;
    return Hit && Hit->GetClass()->HasAnyCastFlag(CASTCLASS_UStructProperty) ? static_cast<UStructProperty*>(Hit)->Struct : nullptr;
}
