#include "pch.h"
#include "UObject/UnrealType.h"
#include "Finder.h"
#include "Natives.h"

FString UProperty::GetCPPType(FString* ExtendedTypeText, uint32_t CPPExportFlags) const
{
    if (Natives::UProperty_GetCPPTypeIndex < 0)
    {
        Finder::ResolvePropertyCPPType();
    }

    using FGetCPPType = FString* (*)(const UProperty* This, FString* Result, FString* ExtendedTypeText, uint32_t CPPExportFlags);
    const FGetCPPType Function = reinterpret_cast<FGetCPPType>((*reinterpret_cast<void* const* const*>(this))[Natives::UProperty_GetCPPTypeIndex]);

    FString Result;
    Function(this, &Result, ExtendedTypeText, CPPExportFlags);
    return Result;
}
