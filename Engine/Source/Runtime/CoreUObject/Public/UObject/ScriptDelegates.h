#pragma once

#include "Containers/Array.h"
#include "UObject/NameTypes.h"
#include "UObject/Object.h"
#include "UObject/WeakObjectPtr.h"

// Epic's TScriptDelegate (ScriptDelegates.h): a dynamic (Blueprint) delegate, an object and the name of a UFunction its
// class has. The engine calls it through FindFunction and ProcessEvent (TMulticastScriptDelegate::ProcessMulticastDelegate,
// 5.40 0x2ECA90), so what it calls must be a UFunction: one of the game's, or Core's own native added to the class with
// UClass::AddNativeFunction.
template <typename TWeakPtr = FWeakObjectPtr>
class TScriptDelegate
{
public:
    TScriptDelegate() = default;

    // Core's shorthand for a default delegate then BindUFunction.
    TScriptDelegate(const UObject* InObject, FName InFunctionName)
        : Object(InObject), FunctionName(InFunctionName)
    {
    }

    void BindUFunction(UObject* InObject, const FName& InFunctionName)
    {
        Object = InObject;
        FunctionName = InFunctionName;
    }

    // Bound to a live object whose class has the function, as the engine checks before it calls one.
    bool IsBound() const
    {
        if (!FunctionName.IsNone())
        {
            if (UObject* ObjectPtr = Object.Get())
            {
                return ObjectPtr->FindFunction(FunctionName) != nullptr;
            }
        }
        return false;
    }

    bool IsBoundToObject(void const* InUserObject) const
    {
        return InUserObject && InUserObject == GetUObject();
    }

    bool IsBoundToObjectEvenIfUnreachable(void const* InUserObject) const
    {
        return InUserObject && InUserObject == GetUObjectEvenIfUnreachable();
    }

    // A multicast delegate drops it: no function, or the object is gone (pending kill still counts as there).
    bool IsCompactable() const
    {
        return FunctionName.IsNone() || !Object.Get(true);
    }

    void Unbind()
    {
        Object.Reset();
        FunctionName = FName();
    }

    void Clear()
    {
        Unbind();
    }

    UObject* GetUObject() const
    {
        return Object.Get();
    }

    UObject* GetUObjectEvenIfUnreachable() const
    {
        return Object.GetEvenIfUnreachable();
    }

    FName GetFunctionName() const
    {
        return FunctionName;
    }

    bool operator==(const TScriptDelegate& Other) const
    {
        return Object == Other.Object && FunctionName == Other.FunctionName;
    }

private:
    TWeakPtr Object;
    FName FunctionName;
};

using FScriptDelegate = TScriptDelegate<>;

static_assert(sizeof(FScriptDelegate) == 0x10);

// Epic's TMulticastScriptDelegate: a BlueprintAssignable delegate (UMulticastDelegateProperty), a TArray of
// TScriptDelegates the engine calls in turn. Core binds its natives to one as Blueprint's AddDynamic does:
//
//     static UFunction* Handler = AFortPlayerPawnAthena::StaticClass()->AddNativeFunction(L"HandlePlayerStartDBNO", &execHandlePlayerStartDBNO, Signature);
//     Pawn->OnPlayerStartDBNO.AddUniqueDynamic(Pawn, Handler->GetFName());
//     Pawn->OnPlayerStartDBNO.RemoveDynamic(Pawn, Handler->GetFName());
template <typename TWeakPtr = FWeakObjectPtr>
class TMulticastScriptDelegate
{
public:
    using FInvocationList = TArray<TScriptDelegate<TWeakPtr>>;

    bool IsBound() const
    {
        return InvocationList.Num() > 0;
    }

    bool Contains(const TScriptDelegate<TWeakPtr>& InDelegate) const
    {
        return InvocationList.Contains(InDelegate);
    }

    bool Contains(const UObject* InObject, FName InFunctionName) const
    {
        return IndexOf(InObject, InFunctionName) >= 0;
    }

    // Compacts first (objects that are gone), then adds InDelegate, even when it is there already.
    void Add(const TScriptDelegate<TWeakPtr>& InDelegate)
    {
        CompactInvocationList();
        InvocationList.Add(InDelegate);
    }

    // Adds InDelegate unless it is there already, then compacts.
    void AddUnique(const TScriptDelegate<TWeakPtr>& InDelegate)
    {
        if (!InvocationList.Contains(InDelegate))
        {
            InvocationList.Add(InDelegate);
        }
        CompactInvocationList();
    }

    void Remove(const TScriptDelegate<TWeakPtr>& InDelegate)
    {
        for (int32_t Index = 0; Index < InvocationList.Num(); ++Index)
        {
            if (InvocationList[Index] == InDelegate)
            {
                InvocationList.RemoveAtSwap(Index);
                break;
            }
        }
        CompactInvocationList();
    }

    void Remove(const UObject* InObject, FName InFunctionName)
    {
        const int32_t Index = IndexOf(InObject, InFunctionName);
        if (Index >= 0)
        {
            InvocationList.RemoveAtSwap(Index);
        }
        CompactInvocationList();
    }

    // Every binding of Object's, and every compactable one.
    void RemoveAll(const UObject* Object)
    {
        for (int32_t Index = InvocationList.Num() - 1; Index >= 0; --Index)
        {
            const TScriptDelegate<TWeakPtr>& Binding = InvocationList[Index];
            if (Binding.IsBoundToObject(Object) || Binding.IsCompactable())
            {
                InvocationList.RemoveAtSwap(Index);
            }
        }
    }

    void Clear()
    {
        InvocationList.Empty();
    }

    // Epic's AddDynamic, AddUniqueDynamic, RemoveDynamic and IsAlreadyBound (TBaseDynamicMulticastDelegate's macros, which
    // name the function from a member function pointer): Core names its native by the name it added it under.
    void AddDynamic(UObject* InUserObject, FName InFunctionName)
    {
        TScriptDelegate<TWeakPtr> NewDelegate;
        NewDelegate.BindUFunction(InUserObject, InFunctionName);
        Add(NewDelegate);
    }

    void AddUniqueDynamic(UObject* InUserObject, FName InFunctionName)
    {
        TScriptDelegate<TWeakPtr> NewDelegate;
        NewDelegate.BindUFunction(InUserObject, InFunctionName);
        AddUnique(NewDelegate);
    }

    void RemoveDynamic(const UObject* InUserObject, FName InFunctionName)
    {
        Remove(InUserObject, InFunctionName);
    }

    bool IsAlreadyBound(const UObject* InUserObject, FName InFunctionName) const
    {
        return Contains(InUserObject, InFunctionName);
    }

private:
    int32_t IndexOf(const UObject* InObject, FName InFunctionName) const
    {
        for (int32_t Index = 0; Index < InvocationList.Num(); ++Index)
        {
            const TScriptDelegate<TWeakPtr>& Delegate = InvocationList[Index];
            if (Delegate.GetFunctionName() == InFunctionName && Delegate.IsBoundToObjectEvenIfUnreachable(InObject))
            {
                return Index;
            }
        }
        return -1;
    }

    void CompactInvocationList()
    {
        for (int32_t Index = InvocationList.Num() - 1; Index >= 0; --Index)
        {
            if (InvocationList[Index].IsCompactable())
            {
                InvocationList.RemoveAtSwap(Index);
            }
        }
    }

    FInvocationList InvocationList;
};

using FMulticastScriptDelegate = TMulticastScriptDelegate<>;

static_assert(sizeof(FMulticastScriptDelegate) == 0x10);
