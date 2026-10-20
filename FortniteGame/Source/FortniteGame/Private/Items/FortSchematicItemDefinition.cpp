#include "pch.h"
#include "Items/FortSchematicItemDefinition.h"
#include "ReflectionParams.h"

UFortWorldItemDefinition* UFortSchematicItemDefinition::GetResultWorldItemDefinition() const
{
    Reflection::FParams Params(StaticFunction_GetResultWorldItemDefinition());
    return Params.Call(const_cast<UFortSchematicItemDefinition*>(this)) ? Params.Take<UFortWorldItemDefinition*>(L"ReturnValue") : nullptr;
}
