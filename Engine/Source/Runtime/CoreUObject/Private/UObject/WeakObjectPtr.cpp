#include "pch.h"
#include "UObject/WeakObjectPtr.h"
#include "Natives.h"
#include "UObject/Object.h"
#include "UObject/UObjectArray.h"

void FWeakObjectPtr::operator=(const UObject* Object)
{
    if (Natives::FWeakObjectPtr_Assign)
    {
        Natives::FWeakObjectPtr_Assign(this, Object);
        return;
    }
    if (Object && Natives::GUObjectArray)
    {
        ObjectIndex = Object->GetUniqueID();
        ObjectSerialNumber = Natives::FUObjectArray_AllocateSerialNumber(Natives::GUObjectArray, ObjectIndex);
    }
    else
    {
        Reset();
    }
}

FUObjectItem* FWeakObjectPtr::Internal_GetObjectItem() const
{
    if (ObjectSerialNumber == 0 || ObjectIndex < 0 || !Natives::GUObjectArray)
    {
        return nullptr;
    }
    FUObjectItem* ObjectItem = Natives::GUObjectArray->IndexToObject(ObjectIndex);
    if (!ObjectItem || ObjectItem->SerialNumber != ObjectSerialNumber)
    {
        return nullptr;
    }
    return ObjectItem;
}

UObject* FWeakObjectPtr::GetEvenIfUnreachable() const
{
    FUObjectItem* const ObjectItem = Internal_GetObjectItem();
    return ObjectItem ? reinterpret_cast<UObject*>(ObjectItem->Object) : nullptr;
}

UObject* FWeakObjectPtr::Get(bool bEvenIfPendingKill) const
{
    FUObjectItem* const ObjectItem = Internal_GetObjectItem();
    if (!ObjectItem)
    {
        return nullptr;
    }
    const bool bValid = bEvenIfPendingKill ? !ObjectItem->IsUnreachable() : !(ObjectItem->IsUnreachable() || ObjectItem->IsPendingKill());
    return bValid ? reinterpret_cast<UObject*>(ObjectItem->Object) : nullptr;
}
