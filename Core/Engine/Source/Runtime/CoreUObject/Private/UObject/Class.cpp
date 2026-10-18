#include "pch.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectArray.h"
#include "Natives.h"

#include <shared_mutex>
#include <unordered_map>

namespace
{
    struct FFieldKey
    {
        const UStruct* Struct;
        FName Name;
        bool bIncludeSuper;

        bool operator==(const FFieldKey& Other) const = default;
    };

    struct FFieldKeyHash
    {
        size_t operator()(const FFieldKey& Key) const noexcept
        {
            return std::hash<const void*>()(Key.Struct) ^ (std::hash<FName>()(Key.Name) * 0x9E3779B97F4A7C15ull) ^ static_cast<size_t>(Key.bIncludeSuper);
        }
    };

    std::shared_mutex FieldCacheMutex;
    std::unordered_map<FFieldKey, UField*, FFieldKeyHash> FieldCache;

    UField* FindField(const UStruct* Struct, FName Name, EClassCastFlags CastFlag, bool bIncludeSuper)
    {
        if (Name.IsNone())
        {
            return nullptr;
        }

        const bool bCacheable = Natives::GUObjectArray && Natives::GUObjectArray->IsDisregardForGC(static_cast<int32_t>(Struct->GetUniqueID()));
        const FFieldKey Key{ Struct, Name, bIncludeSuper };

        if (bCacheable)
        {
            std::shared_lock Lock(FieldCacheMutex);
            if (const auto It = FieldCache.find(Key); It != FieldCache.end())
            {
                return It->second;
            }
        }

        UField* Found = nullptr;
        for (const UStruct* Current = Struct; Current && !Found; Current = bIncludeSuper ? Current->GetSuperStruct() : nullptr)
        {
            for (UField* Field = Current->Children; Field; Field = Field->Next)
            {
                if (Field->GetFName() == Name && Field->GetClass()->HasAnyCastFlag(CastFlag))
                {
                    Found = Field;
                    break;
                }
            }
        }

        if (bCacheable)
        {
            std::unique_lock Lock(FieldCacheMutex);
            FieldCache.emplace(Key, Found);
        }

        return Found;
    }
}

bool UStruct::IsChildOf(const UStruct* SomeBase) const
{
    for (const UStruct* Struct = this; Struct; Struct = Struct->GetSuperStruct())
    {
        if (Struct == SomeBase)
        {
            return true;
        }
    }

    return false;
}

UProperty* UStruct::FindPropertyByName(FName InName) const
{
    return static_cast<UProperty*>(FindField(this, InName, CASTCLASS_UProperty, true));
}

UFunction* UClass::FindFunctionByName(FName InName, EIncludeSuperFlag::Type IncludeSuper) const
{
    return static_cast<UFunction*>(FindField(this, InName, CASTCLASS_UFunction, IncludeSuper == EIncludeSuperFlag::IncludeSuper));
}
