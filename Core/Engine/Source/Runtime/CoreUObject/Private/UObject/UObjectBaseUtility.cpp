#include "pch.h"
#include "UObject/UObjectBaseUtility.h"
#include "UObject/Class.h"
#include "UObject/UObjectArray.h"
#include "Natives.h"

namespace
{
    FUObjectItem* GetObjectItem(const UObjectBaseUtility* Object)
    {
        return Natives::GUObjectArray ? Natives::GUObjectArray->IndexToObject(static_cast<int32_t>(Object->GetUniqueID())) : nullptr;
    }

    void GetPathNameInternal(const UObjectBaseUtility* Object, const UObject* StopOuter, FString& ResultString)
    {
        if (Object != reinterpret_cast<const UObjectBaseUtility*>(StopOuter) && Object != nullptr)
        {
            const UObject* ObjOuter = Object->GetOuter();
            if (ObjOuter && ObjOuter != StopOuter)
            {
                GetPathNameInternal(ObjOuter, StopOuter, ResultString);

                const UObject* OuterOuter = ObjOuter->GetOuter();
                if (!ObjOuter->GetClass()->HasAnyCastFlag(CASTCLASS_UPackage) && OuterOuter && OuterOuter->GetClass()->HasAnyCastFlag(CASTCLASS_UPackage))
                {
                    ResultString += L':';
                }
                else
                {
                    ResultString += L'.';
                }
            }

            ResultString += Object->GetName();
        }
        else
        {
            ResultString += L"None";
        }
    }
}

bool UObjectBaseUtility::IsTemplate(EObjectFlags TemplateTypes) const
{
    for (const UObjectBaseUtility* TestOuter = this; TestOuter; TestOuter = TestOuter->GetOuter())
    {
        if (TestOuter->HasAnyFlags(TemplateTypes))
        {
            return true;
        }
    }

    return false;
}

void UObjectBaseUtility::MarkPendingKill()
{
    if (FUObjectItem* Item = GetObjectItem(this))
    {
        Item->SetPendingKill();
    }
}

bool UObjectBaseUtility::IsPendingKill() const
{
    const FUObjectItem* Item = GetObjectItem(this);
    return Item && Item->IsPendingKill();
}

void UObjectBaseUtility::AddToRoot()
{
    if (FUObjectItem* Item = GetObjectItem(this))
    {
        Item->SetRootSet();
    }
}

void UObjectBaseUtility::RemoveFromRoot()
{
    if (FUObjectItem* Item = GetObjectItem(this))
    {
        Item->ClearRootSet();
    }
}

bool UObjectBaseUtility::IsRooted() const
{
    const FUObjectItem* Item = GetObjectItem(this);
    return Item && Item->IsRootSet();
}

FString UObjectBaseUtility::GetPathName(const UObject* StopOuter) const
{
    FString Result;
    GetPathNameInternal(this, StopOuter, Result);
    return Result;
}

FString UObjectBaseUtility::GetFullName(const UObject* StopOuter) const
{
    FString Result = GetClass()->GetName();
    Result += L" ";
    GetPathNameInternal(this, StopOuter, Result);
    return Result;
}

UObject* UObjectBaseUtility::GetOutermost() const
{
    const UObjectBaseUtility* Top = this;
    while (Top->GetOuter())
    {
        Top = Top->GetOuter();
    }

    return const_cast<UObject*>(static_cast<const UObject*>(Top));
}

bool UObjectBaseUtility::IsA(const UClass* SomeBase) const
{
    return SomeBase && GetClass()->IsChildOf(SomeBase);
}
