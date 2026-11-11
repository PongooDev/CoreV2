#include "pch.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectArray.h"
#include "Finder.h"
#include "Log.h"
#include "Natives.h"
#include "UObject/UObjectGlobals.h"

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

void UStruct::InitializeStruct(void* Dest, int32_t ArrayDim) const
{
    if (Natives::UStruct_InitializeStructIndex < 0)
    {
        Finder::ResolveScriptStructCopy();
    }

    using FInitializeStruct = void (*)(const UStruct* This, void* Dest, int32_t ArrayDim);
    reinterpret_cast<FInitializeStruct>((*reinterpret_cast<void* const* const*>(this))[Natives::UStruct_InitializeStructIndex])(this, Dest, ArrayDim);
}

void UStruct::DestroyStruct(void* Dest, int32_t ArrayDim) const
{
    if (Natives::UStruct_DestroyStructIndex < 0)
    {
        Finder::ResolveScriptStructCopy();
    }

    using FDestroyStruct = void (*)(const UStruct* This, void* Dest, int32_t ArrayDim);
    reinterpret_cast<FDestroyStruct>((*reinterpret_cast<void* const* const*>(this))[Natives::UStruct_DestroyStructIndex])(this, Dest, ArrayDim);
}

int32_t UStruct::GetStructureSize() const
{
    const int32_t Alignment = GetMinAlignment();
    return (GetPropertiesSize() + Alignment - 1) & ~(Alignment - 1);
}

void UScriptStruct::CopyScriptStruct(void* Dest, void const* Src, int32_t ArrayDim) const
{
    if (!Natives::UScriptStruct_CopyScriptStruct)
    {
        Finder::ResolveScriptStructCopy();
    }

    Natives::UScriptStruct_CopyScriptStruct(this, Dest, Src, ArrayDim);
}

UFunction* UClass::FindFunctionByName(FName InName, EIncludeSuperFlag::Type IncludeSuper) const
{
    return static_cast<UFunction*>(FindField(this, InName, CASTCLASS_UFunction, IncludeSuper == EIncludeSuperFlag::IncludeSuper));
}

void UClass::AddFunctionToFunctionMap(UFunction* Function, FName FuncName)
{
    GetFuncMap().Add(FuncName, Function);
}

UFunction* UClass::AddNativeFunction(const wchar_t* InName, FNativeFuncPtr InPointer, const UFunction* SignatureFunction)
{
    const FName FunctionName(InName);
    // Its native's parameters are read through the FFrame (P_GET_*, P_STACK.Locals).
    if (!Finder::ResolveClassFunctionMap() || ((Natives::FFrame_Locals < 0 || !Natives::FFrame_StepExplicitProperty) && !Finder::ResolveScriptFrame()))
    {
        Log::Error(L"Delegates: Core's native {}::{} can't be added (UClass::FuncMap or FFrame not found)", GetName().ToStdString(), InName);
        return nullptr;
    }
    if (UFunction* const* Existing = GetFuncMap().Find(FunctionName))
    {
        if ((*Existing)->GetNativeFunc() == InPointer)
        {
            return *Existing;
        }
        Log::Error(L"Delegates: {} already has a function named {}, Core's native isn't added", GetName().ToStdString(), InName);
        return nullptr;
    }

    // UECodeGen_Private::ConstructUFunction: a native function, public and final as a UFUNCTION() is, outered to its class
    // (UFunction::Invoke reads its outer as the class). Rooted: nothing else references it for the garbage collector.
    UFunction* Function = NewObject<UFunction>(this, UFunction::StaticClass(), FunctionName, RF_Public | RF_Transient);
    if (!Function)
    {
        Log::Error(L"Delegates: Core's native {}::{} couldn't be constructed", GetName().ToStdString(), InName);
        return nullptr;
    }
    Function->AddToRoot();

    // Its parameters are the signature's own properties (Children), so ProcessEvent sizes its frame from them, points
    // the out parameters at the caller's (FUNC_HasOutParms) and the thunk steps through them as a compiled-in one does.
    // DestructorLink stays empty: the frame's copies of the caller's parameters aren't destroyed twice.
    EFunctionFlags Flags = static_cast<EFunctionFlags>(FUNC_Final | FUNC_Native | FUNC_Public);
    if (SignatureFunction)
    {
        Flags = static_cast<EFunctionFlags>(Flags | (SignatureFunction->FunctionFlags & FUNC_HasOutParms));
        Function->Children = SignatureFunction->Children;
        Function->PropertiesSize = SignatureFunction->PropertiesSize;
        Function->MinAlignment = SignatureFunction->MinAlignment;
        Function->NumParms = SignatureFunction->NumParms;
        Function->ParmsSize = SignatureFunction->ParmsSize;
        Function->ReturnValueOffset = SignatureFunction->ReturnValueOffset;
    }
    else
    {
        Function->MinAlignment = 1;
        Function->ReturnValueOffset = 0xFFFF;
    }
    Function->FunctionFlags = Flags;
    Function->SetNativeFunc(InPointer);

    // UClass::CreateLinkAndAddChildFunctionsToMap.
    Function->Next = Children;
    Children = Function;
    AddFunctionToFunctionMap(Function, FunctionName);

    Log::Info(L"Delegates: Core's native {}::{} added ({}, {} bytes of parameters)", GetName().ToStdString(), InName,
        SignatureFunction ? SignatureFunction->GetName().ToStdString() : std::wstring(L"no signature"), static_cast<int32_t>(Function->ParmsSize));
    return Function;
}

UObject* UClass::CreateDefaultObject()
{
    if (Natives::UClass_CreateDefaultObjectIndex < 0)
    {
        return ClassDefaultObject;
    }

    using FCreateDefaultObject = UObject* (*)(UClass*);
    void** VTable = *reinterpret_cast<void***>(this);
    return reinterpret_cast<FCreateDefaultObject>(VTable[Natives::UClass_CreateDefaultObjectIndex])(this);
}
