#include "pch.h"
#include "Kismet/KismetNodeHelperLibrary.h"
#include "ReflectionParams.h"

FName UKismetNodeHelperLibrary::GetEnumeratorName(const UEnum* Enum, uint8_t EnumeratorValue)
{
    Reflection::FParams Params(StaticFunction_GetEnumeratorName());
    Params.Set(L"Enum", Enum);
    Params.Set(L"EnumeratorValue", EnumeratorValue);
    return Params.Call(StaticClass()->GetDefaultObject()) ? Params.Take<FName>(L"ReturnValue") : FName();
}
