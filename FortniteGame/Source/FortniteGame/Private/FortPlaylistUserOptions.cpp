#include "pch.h"
#include "FortPlaylistUserOptions.h"
#include "Natives.h"
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

FString UPlaylistUserOptionBase::GetOptionValueFromName(const FString& OptionName) const
{
    FString Value;
    if (Natives::UPlaylistUserOptionBase_GetOptionValueFromName)
    {
        Natives::UPlaylistUserOptionBase_GetOptionValueFromName(this, &Value, &OptionName);
    }
    return Value;
}

UPlaylistUserOptionBase* UPlaylistUserOptions::GetOption(const FString& OptionKey) const
{
    for (UPlaylistUserOptionBase* Option : Options)
    {
        if (Option && Option->GetOptionKey(true) == OptionKey)
        {
            return Option;
        }
    }
    return nullptr;
}
