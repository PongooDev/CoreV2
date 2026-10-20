#include "pch.h"
#include "Items/FortItem.h"
#include "Items/FortItemDefinition.h"
#include "Finder.h"
#include "Natives.h"
#include "ReflectionParams.h"

UFortItem* UFortItemDefinition::CreateTemporaryItemInstanceBP(int32_t Count, int32_t Level) const
{
    Reflection::FParams Params(StaticFunction_CreateTemporaryItemInstanceBP());
    Params.Set(L"Count", Count);
    Params.Set(L"Level", Level);
    return Params.Call(const_cast<UFortItemDefinition*>(this)) ? Params.Take<UFortItem*>(L"ReturnValue") : nullptr;
}

void FFortItemEntry::Construct(FFortItemEntry* This, const UFortItemDefinition* InItemDefinition, int32_t InCount, int32_t InLevel)
{
    if (!Natives::FFortItemEntry_FFortItemEntry && !Finder::ResolvePickupLive())
    {
        StaticStruct()->InitializeStruct(This);
        return;
    }
    Natives::FFortItemEntry_FFortItemEntry(This, InItemDefinition, InCount, InLevel, nullptr);
}

bool UFortWorldItemDefinition::ServerExecute(const UFortItem* Item, AFortPlayerController* Controller)
{
    if (Natives::UFortWorldItemDefinition_ServerExecuteIndex < 0 && !Finder::ResolveWorldItemDefinitionLive())
    {
        return false;
    }

    using FServerExecute = bool (*)(UFortWorldItemDefinition* This, const UFortItem* Item, AFortPlayerController* Controller);
    return reinterpret_cast<FServerExecute>((*reinterpret_cast<void* const* const*>(this))[Natives::UFortWorldItemDefinition_ServerExecuteIndex])(this, Item, Controller);
}

UFortItemDefinition* UFortItem::GetItemDefinitionBP() const
{
    Reflection::FParams Params(StaticFunction_GetItemDefinitionBP());
    return Params.Call(const_cast<UFortItem*>(this)) ? Params.Take<UFortItemDefinition*>(L"ReturnValue") : nullptr;
}

int32_t UFortItem::GetLevel() const
{
    Reflection::FParams Params(StaticFunction_GetLevel());
    return Params.Call(const_cast<UFortItem*>(this)) ? Params.Take<int32_t>(L"ReturnValue") : 0;
}

void UFortWorldItem::SetOwningControllerForTemporaryItem(AFortPlayerController* InController)
{
    Reflection::FParams Params(StaticFunction_SetOwningControllerForTemporaryItem());
    Params.Set(L"InController", InController);
    Params.Call(this);
}
