#pragma once

#include "Finder.h"
#include "Natives.h"
#include "Reflection.h"
#include "UObject/NameTypes.h"
#include "UObject/ScriptDelegates.h"

class UObject;

// UE 4.23's sparse dynamic multicast delegates (8.20, CL-5625478: AActor::OnDestroyed and OnEndPlay,
// UMulticastSparseDelegateProperty). The member is one byte, bIsBound; FSparseDelegateStorage keeps the bindings in one
// map by the delegate's owner and name. 8.20's own AddDynamic of an actor's OnDestroyed (0x1489D83): Add(Actor,
// FName "OnDestroyed", Delegate), then "or [actor+1AEh], al"; its RemoveDynamic (0x76963C): when bound, Remove(Actor,
// Name, UserObject, FunctionName) into bIsBound.
class FSparseDelegateStorage
{
public:
    static bool IsAvailable()
    {
        return Natives::FSparseDelegateStorage_Add && Natives::FSparseDelegateStorage_Remove;
    }

    // Whether the delegate is bound afterwards.
    static bool Add(const UObject* DelegateOwner, FName DelegateName, FScriptDelegate Delegate)
    {
        return Natives::FSparseDelegateStorage_Add(DelegateOwner, DelegateName, &Delegate);
    }

    static bool Remove(const UObject* DelegateOwner, FName DelegateName, const UObject* UserObject, FName FunctionName)
    {
        return Natives::FSparseDelegateStorage_Remove(DelegateOwner, DelegateName, UserObject, FunctionName);
    }
};

struct FSparseDelegate
{
    bool bIsBound = false;
};

// A BlueprintAssignable member as the build has it: inline (the TMulticastScriptDelegate itself), or sparse from 8.20.
// Binds as TBaseDynamicMulticastDelegate's and TSparseDynamicDelegate's AddUniqueDynamic and RemoveDynamic do.
class FDynamicMulticastDelegateRef
{
public:
    FDynamicMulticastDelegateRef(const UObject* InOwner, FName InName, FMulticastScriptDelegate* InInline, FSparseDelegate* InSparse)
        : Owner(InOwner), Name(InName), Inline(InInline), Sparse(InSparse)
    {
    }

    void AddUniqueDynamic(UObject* InUserObject, FName InFunctionName) const
    {
        if (Inline)
        {
            Inline->AddUniqueDynamic(InUserObject, InFunctionName);
            return;
        }
        if (!Sparse || !FSparseDelegateStorage::IsAvailable())
        {
            return;
        }
        // FSparseDelegateStorage::AddUnique isn't in the image; Remove then Add leaves the same one binding.
        RemoveDynamic(InUserObject, InFunctionName);
        FScriptDelegate NewDelegate;
        NewDelegate.BindUFunction(InUserObject, InFunctionName);
        Sparse->bIsBound |= FSparseDelegateStorage::Add(Owner, Name, NewDelegate);
    }

    void RemoveDynamic(const UObject* InUserObject, FName InFunctionName) const
    {
        if (Inline)
        {
            Inline->RemoveDynamic(InUserObject, InFunctionName);
            return;
        }
        if (Sparse && Sparse->bIsBound && FSparseDelegateStorage::IsAvailable())
        {
            Sparse->bIsBound = FSparseDelegateStorage::Remove(Owner, Name, InUserObject, InFunctionName);
        }
    }

private:
    const UObject* Owner;
    FName Name;
    FMulticastScriptDelegate* Inline;
    FSparseDelegate* Sparse;
};

// A dynamic multicast delegate member that is inline on some builds and sparse on others (AActor's OnDestroyed and
// OnEndPlay from 8.20). Has##Name() is whether Core can bind it on this build.
#define DefineDynamicMulticastDelegateUProperty(Name) \
private: \
    static inline Reflection::FMemberInfo Member_##Name{ &StaticReflectionOwner, TEXT(#Name), sizeof(FMulticastScriptDelegate), Reflection::EMemberKind::Value }; \
    static inline Reflection::FMemberInfo Member_##Name##Sparse{ &StaticReflectionOwner, TEXT(#Name), sizeof(FSparseDelegate), Reflection::EMemberKind::Value }; \
public: \
    FDynamicMulticastDelegateRef GetMember_##Name() const \
    { \
        FMulticastScriptDelegate* InlineDelegate = Member_##Name.IsAvailable() ? &Reflection::AccessMember<FMulticastScriptDelegate>(this, Member_##Name) : nullptr; \
        FSparseDelegate* SparseDelegate = !InlineDelegate && Member_##Name##Sparse.IsAvailable() ? &Reflection::AccessMember<FSparseDelegate>(this, Member_##Name##Sparse) : nullptr; \
        return FDynamicMulticastDelegateRef(reinterpret_cast<const UObject*>(this), FName(TEXT(#Name)), InlineDelegate, SparseDelegate); \
    } \
    static bool Has##Name() \
    { \
        return StaticClass() && (Member_##Name.IsAvailable() || (Member_##Name##Sparse.IsAvailable() && Finder::ResolveSparseDelegateStorageLive(TEXT(#Name), Member_##Name##Sparse.GetOffsetChecked()))); \
    } \
    __declspec(property(get = GetMember_##Name)) FDynamicMulticastDelegateRef Name;
