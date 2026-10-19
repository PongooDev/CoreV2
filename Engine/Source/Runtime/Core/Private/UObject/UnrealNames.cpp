#include "pch.h"
#include "UObject/NameTypes.h"
#include "Natives.h"

FName::FName(const wchar_t* Name, EFindName FindType)
{
    Natives::FName_Construct(this, Name, FindType);
}

FString FName::ToString() const
{
    FString Out;
    Natives::FName_AppendString(this, Out);
    return Out;
}

std::wstring FName::ToStdString() const
{
    return ToString().ToStdString();
}
