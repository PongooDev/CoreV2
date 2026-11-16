#include "pch.h"
#include "Kismet/KismetTextLibrary.h"
#include "ReflectionParams.h"

FText UKismetTextLibrary::Conv_StringToText(const FString& InString)
{
    // The buffer isn't initialized (FParams' false), so the returned text is only moved out of it, never destroyed there.
    UFunction* Function = FindStaticFunction_Conv_StringToText();
    Reflection::FParams Params(Function);
    if (!Function || !Params.Set(L"inString", InString) || !Params.Call(StaticClass()->GetDefaultObject()))
    {
        return FText();
    }
    return Params.Take<FText>(L"ReturnValue");
}

FString UKismetTextLibrary::Conv_TextToString(const FText& InText)
{
    UFunction* Function = FindStaticFunction_Conv_TextToString();
    Reflection::FParams Params(Function);
    if (!Function || !InText.HasTextData() || !Params.Set(L"InText", InText) || !Params.Call(StaticClass()->GetDefaultObject()))
    {
        return FString();
    }
    return Params.Take<FString>(L"ReturnValue");
}
