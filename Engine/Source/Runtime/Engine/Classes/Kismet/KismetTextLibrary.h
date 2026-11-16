#pragma once

#include "Containers/UnrealString.h"
#include "Internationalization/Text.h"
#include "UObject/Object.h"

class UKismetTextLibrary : public UObject
{
    DefineUnrealClass(UKismetTextLibrary, Engine)
    DefineUFunction(Conv_StringToText)
    DefineUFunction(Conv_TextToString)

public:
    // A culture invariant text of the string (FText::FromString). One without text data when the call fails.
    static FText Conv_StringToText(const FString& InString);
    static FString Conv_TextToString(const FText& InText);
};
