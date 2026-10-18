#include "pch.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectArray.h"
#include "UObject/UObjectGlobals.h"
#include "Natives.h"

#include <shared_mutex>
#include <unordered_map>

namespace
{
    struct FFieldKey
    {
        const UStruct* Struct;
        FName Name;

        bool operator==(const FFieldKey& Other) const = default;
    };

    struct FFieldKeyHash
    {
        size_t operator()(const FFieldKey& Key) const noexcept
        {
            return std::hash<const void*>()(Key.Struct) ^ (std::hash<FName>()(Key.Name) * 0x9E3779B97F4A7C15ull);
        }
    };

    std::shared_mutex FieldCacheMutex;
    std::unordered_map<FFieldKey, UField*, FFieldKeyHash> FieldCache;

    bool IsPermanent(const UStruct* Struct)
    {
        return Natives::GUObjectArray && Struct->InternalIndex <= Natives::GUObjectArray->ObjLastNonGCIndex;
    }

    UField* FindField(const UStruct* Struct, FName Name, EClassCastFlags CastFlags)
    {
        if (!Struct || Name.IsNone())
        {
            return nullptr;
        }

        const bool bCacheable = IsPermanent(Struct);
        const FFieldKey Key{ Struct, Name };

        if (bCacheable)
        {
            std::shared_lock Lock(FieldCacheMutex);
            if (const auto It = FieldCache.find(Key); It != FieldCache.end())
            {
                return It->second;
            }
        }

        UField* Found = nullptr;
        for (const UStruct* Current = Struct; Current && !Found; Current = Current->GetSuperStruct())
        {
            for (UField* Field = Current->GetChildren(); Field; Field = Field->GetNext())
            {
                if (Field->GetFName() == Name && Field->GetClass()->HasAnyCastFlag(CastFlags))
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

bool UStruct::IsChildOf(const UStruct* Base) const
{
    for (const UStruct* Current = this; Current; Current = Current->GetSuperStruct())
    {
        if (Current == Base)
        {
            return true;
        }
    }

    return false;
}

UProperty* UStruct::FindPropertyByName(FName Name) const
{
    return static_cast<UProperty*>(FindField(this, Name, EClassCastFlags::Property));
}

UFunction* UStruct::FindFunctionByName(FName Name) const
{
    return static_cast<UFunction*>(FindField(this, Name, EClassCastFlags::Function));
}

UClass* UClass::StaticClass()
{
    static UClass* Class = nullptr;
    if (!Class)
    {
        Class = FindObject<UClass>(L"/Script/CoreUObject.Class");
    }

    return Class;
}
