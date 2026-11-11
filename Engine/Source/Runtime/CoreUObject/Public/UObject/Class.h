#pragma once

#include "Containers/Map.h"
#include "ObjectLayout.h"
#include "UObject/Object.h"

class UFunction;
class UProperty;

// Epic's FNativeFuncPtr (4.20+; a UObject member function pointer before, with the same calling convention): a native
// UFunction's thunk, DECLARE_FUNCTION's signature. TheStack is the FFrame.
using FNativeFuncPtr = void (*)(UObject* Context, void* TheStack, void* Result);

namespace EIncludeSuperFlag
{
    enum Type
    {
        ExcludeSuper,
        IncludeSuper,
    };
}

class UField : public UObject
{
    DefineUnrealClass(UField, CoreUObject)
    DefineLayoutMember(UField*, Next, UField_Next)
};

class UStruct : public UField
{
    DefineUnrealClass(UStruct, CoreUObject)
    DefineLayoutMember(UField*, Children, UStruct_Children)
    DefineLayoutMember(int32_t, PropertiesSize, UStruct_PropertiesSize)
    DefineLayoutMember(int32_t, MinAlignment, UStruct_MinAlignment)

public:
    UStruct* GetSuperStruct() const
    {
        return *reinterpret_cast<UStruct* const*>(reinterpret_cast<uintptr_t>(this) + ObjectLayout::Current.UStruct_SuperStruct);
    }

    int32_t GetPropertiesSize() const { return PropertiesSize; }
    int32_t GetMinAlignment() const { return MinAlignment; }

    // PropertiesSize aligned to MinAlignment: what InitializeStruct, DestroyStruct and CopyScriptStruct touch. A
    // function's includes its local variables, past ParmsSize.
    int32_t GetStructureSize() const;

    bool IsChildOf(const UStruct* SomeBase) const;
    UProperty* FindPropertyByName(FName InName) const;

    void InitializeStruct(void* Dest, int32_t ArrayDim = 1) const;
    void DestroyStruct(void* Dest, int32_t ArrayDim = 1) const;
};

class UScriptStruct : public UStruct
{
    DefineUnrealClass(UScriptStruct, CoreUObject)

public:
    void CopyScriptStruct(void* Dest, void const* Src, int32_t ArrayDim = 1) const;
};

class UClass : public UStruct
{
    DefineUnrealClass(UClass, CoreUObject)
    DefineLayoutMember(EClassCastFlags, ClassCastFlags, UClass_ClassCastFlags)
    DefineLayoutMember(UObject*, ClassDefaultObject, UClass_ClassDefaultObject)

public:
    UClass* GetSuperClass() const { return static_cast<UClass*>(GetSuperStruct()); }
    bool HasAnyCastFlag(EClassCastFlags FlagToCheck) const { return (ClassCastFlags & FlagToCheck) != 0; }
    bool HasAllCastFlags(EClassCastFlags FlagsToCheck) const { return (ClassCastFlags & FlagsToCheck) == FlagsToCheck; }

    UObject* GetDefaultObject(bool bCreateIfNeeded = true) const
    {
        if (ClassDefaultObject == nullptr && bCreateIfNeeded)
        {
            const_cast<UClass*>(this)->CreateDefaultObject();
        }

        return ClassDefaultObject;
    }

    UObject* CreateDefaultObject();

    UFunction* FindFunctionByName(FName InName, EIncludeSuperFlag::Type IncludeSuper = EIncludeSuperFlag::IncludeSuper) const;

    // Epic's UClass::AddFunctionToFunctionMap: FuncMap, where the engine's FindFunctionByName looks a name up first (a
    // Blueprint delegate's ProcessMulticastDelegate, K2_SetTimer, a console exec). Needs Finder::ResolveClassFunctionMap.
    void AddFunctionToFunctionMap(UFunction* Function, FName FuncName);

    // A native UFunction named InName on this class, its thunk InPointer (DEFINE_FUNCTION), for Core to bind to the game's
    // Blueprint delegates (FMulticastScriptDelegate::AddDynamic) and timers by name. Epic's AddNativeFunction only
    // records the thunk for UHT's functions, which UECodeGen_Private::ConstructUFunction makes and
    // CreateLinkAndAddChildFunctionsToMap links into the class (Children, FuncMap) when the class is constructed; the
    // game's classes are long constructed, so this does all three. SignatureFunction (a delegate property's
    // SignatureFunction) gives it the delegate's parameters: its native reads them as a compiled-in one does (P_GET_*,
    // P_FINISH), or as the signature's parameter struct at P_STACK.Locals. Null: no parameters. Returns the function
    // (the existing one when called again with the same thunk), or null where it can't be added (logged). Call it on the
    // game thread at boot (HookNatives / HookVTables): FuncMap has no lock, and the loader reads it on its own thread.
    UFunction* AddNativeFunction(const wchar_t* InName, FNativeFuncPtr InPointer, const UFunction* SignatureFunction = nullptr);

private:
    TMap<FName, UFunction*>& GetFuncMap() const
    {
        return *reinterpret_cast<TMap<FName, UFunction*>*>(reinterpret_cast<uintptr_t>(this) + ObjectLayout::Current.UClass_FuncMap);
    }
};

class UFunction : public UStruct
{
    DefineUnrealClass(UFunction, CoreUObject)
    DefineLayoutMember(EFunctionFlags, FunctionFlags, UFunction_FunctionFlags)
    DefineLayoutMember(uint8_t, NumParms, UFunction_NumParms)
    DefineLayoutMember(uint16_t, ParmsSize, UFunction_ParmsSize)
    DefineLayoutMember(uint16_t, ReturnValueOffset, UFunction_ReturnValueOffset)

public:
    using FNativeFuncPtr = ::FNativeFuncPtr;

    FNativeFuncPtr GetNativeFunc() const
    {
        return *reinterpret_cast<const FNativeFuncPtr*>(reinterpret_cast<uintptr_t>(this) + ObjectLayout::Current.UFunction_Func);
    }

    void SetNativeFunc(FNativeFuncPtr InFunc)
    {
        *reinterpret_cast<FNativeFuncPtr*>(reinterpret_cast<uintptr_t>(this) + ObjectLayout::Current.UFunction_Func) = InFunc;
    }

    bool HasAnyFunctionFlags(EFunctionFlags FlagsToCheck) const { return (FunctionFlags & FlagsToCheck) != 0; }

    UFunction* GetSuperFunction() const { return static_cast<UFunction*>(GetSuperStruct()); }
};

class UEnum : public UField
{
    DefineUnrealClass(UEnum, CoreUObject)
};
