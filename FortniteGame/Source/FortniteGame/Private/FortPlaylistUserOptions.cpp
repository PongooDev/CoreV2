#include "pch.h"
#include "FortPlaylistUserOptions.h"
#include "ReflectionParams.h"

FString UPlaylistUserOptionBase::GetOptionKey(bool bWithScoping) const
{
    UFunction* Function = StaticClass() ? FindStaticFunction_GetOptionKey() : nullptr;
    if (!Function)
    {
        return FString();
    }
    Reflection::FParams Params(Function, true);
    Params.Set(L"bWithScoping", bWithScoping);
    return Params.Call(const_cast<UPlaylistUserOptionBase*>(this)) ? Params.Take<FString>(L"ReturnValue") : FString();
}
