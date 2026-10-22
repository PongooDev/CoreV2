#pragma once

#include "UObject/Object.h"

class UEnum;

class UKismetNodeHelperLibrary : public UObject
{
    DefineUnrealClass(UKismetNodeHelperLibrary, Engine)
    DefineUFunction(GetEnumeratorName)

public:
    static FName GetEnumeratorName(const UEnum* Enum, uint8_t EnumeratorValue);
};
