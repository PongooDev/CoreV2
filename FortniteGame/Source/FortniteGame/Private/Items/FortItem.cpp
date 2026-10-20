#include "pch.h"
#include "Items/FortItem.h"
#include "Items/FortItemDefinition.h"
#include "ReflectionParams.h"

UFortItem* UFortItemDefinition::CreateTemporaryItemInstanceBP(int32_t Count, int32_t Level) const
{
    Reflection::FParams Params(StaticFunction_CreateTemporaryItemInstanceBP());
    Params.Set(L"Count", Count);
    Params.Set(L"Level", Level);
    return Params.Call(const_cast<UFortItemDefinition*>(this)) ? Params.Take<UFortItem*>(L"ReturnValue") : nullptr;
}

void UFortWorldItem::SetOwningControllerForTemporaryItem(AFortPlayerController* InController)
{
    Reflection::FParams Params(StaticFunction_SetOwningControllerForTemporaryItem());
    Params.Set(L"InController", InController);
    Params.Call(this);
}
