#pragma once

#include "Delegates/DelegateBase.h"

#include <cstring>
#include <tuple>
#include <type_traits>
#include <utility>

template <typename FuncType>
class IBaseDelegateInstance;

template <typename RetType, typename... ParamTypes>
class IBaseDelegateInstance<RetType(ParamTypes...)> : public IDelegateInstance
{
public:
    virtual void CreateCopy(FDelegateBase& Base) = 0;
    virtual bool IsSameFunction(const IBaseDelegateInstance& InOtherDelegate) const = 0;
    virtual RetType Execute(ParamTypes... Params) const = 0;
    virtual bool ExecuteIfSafe(ParamTypes... Params) const = 0;
};

namespace DelegateInstance
{
    // IBaseDelegateInstance's virtuals at the build's slots (see EDelegateInstanceSlot). Core's delegates return void.
    template <typename RetType, typename... ParamTypes>
    void CreateCopy(IBaseDelegateInstance<RetType(ParamTypes...)>* Instance, FDelegateBase& Base)
    {
        CallVirtual<void, IBaseDelegateInstance<RetType(ParamTypes...)>, FDelegateBase&>(Instance, Slot(EDelegateInstanceSlot::CreateCopy), Base);
    }

    template <typename RetType, typename... ParamTypes>
    RetType Execute(const IBaseDelegateInstance<RetType(ParamTypes...)>* Instance, std::type_identity_t<ParamTypes>... Params)
    {
        static_assert(std::is_void_v<RetType> || std::is_scalar_v<RetType>, "a class return value is passed through a hidden pointer");
        return CallVirtual<RetType, const IBaseDelegateInstance<RetType(ParamTypes...)>, ParamTypes...>(Instance, Slot(EDelegateInstanceSlot::Execute), Params...);
    }

    template <typename RetType, typename... ParamTypes>
    bool ExecuteIfSafe(const IBaseDelegateInstance<RetType(ParamTypes...)>* Instance, std::type_identity_t<ParamTypes>... Params)
    {
        return CallVirtual<bool, const IBaseDelegateInstance<RetType(ParamTypes...)>, ParamTypes...>(Instance, Slot(EDelegateInstanceSlot::ExecuteIfSafe), Params...);
    }
}

template <typename FuncType, typename... VarTypes>
class TBaseStaticDelegateInstance;

template <typename RetType, typename... ParamTypes, typename... VarTypes>
class TBaseStaticDelegateInstance<RetType(ParamTypes...), VarTypes...> : public IBaseDelegateInstance<RetType(ParamTypes...)>
{
public:
    using FFuncPtr = RetType (*)(ParamTypes..., VarTypes...);

    TBaseStaticDelegateInstance(FFuncPtr InStaticFuncPtr, VarTypes... Vars)
        : StaticFuncPtr(InStaticFuncPtr), Payload(Vars...), Handle(FDelegateHandle::GenerateNewHandle)
    {
        UseBuildVTable();
    }

    TBaseStaticDelegateInstance(const TBaseStaticDelegateInstance& Other)
        : StaticFuncPtr(Other.StaticFuncPtr), Payload(Other.Payload), Handle(Other.Handle)
    {
        UseBuildVTable();
    }

    FName GetFunctionName() const final { return FName(); }
    UObject* GetUObject() const final { return nullptr; }
    const void* GetRawMethodPtr() const final { return reinterpret_cast<const void*>(StaticFuncPtr); }
    const void* GetRawUserObject() const final { return nullptr; }
    EDelegateInstanceType::Type GetType() const final { return EDelegateInstanceType::Raw; }
    bool HasSameObject(const void*) const final { return false; }
    // Not IDelegateInstance's !IsSafeToExecute(): that is a virtual call, and 4.20's vtable has Execute at its slot.
    bool IsCompactable() const final { return !TBaseStaticDelegateInstance::IsSafeToExecute(); }
    bool IsSafeToExecute() const final { return true; }
    FDelegateHandle GetHandle() const final { return Handle; }

    void CreateCopy(FDelegateBase& Base) final
    {
        new (Base) TBaseStaticDelegateInstance(*this);
    }

    bool IsSameFunction(const IBaseDelegateInstance<RetType(ParamTypes...)>& InOtherDelegate) const final
    {
        return InOtherDelegate.GetType() == EDelegateInstanceType::Raw && InOtherDelegate.GetRawMethodPtr() == TBaseStaticDelegateInstance::GetRawMethodPtr();
    }

    RetType Execute(ParamTypes... Params) const final
    {
        return std::apply([&](auto&... Vars) { return StaticFuncPtr(Params..., Vars...); }, Payload);
    }

    bool ExecuteIfSafe(ParamTypes... Params) const final
    {
        TBaseStaticDelegateInstance::Execute(Params...);
        return true;
    }

private:
    // On 4.20 the engine calls this instance by 4.20's slots: point it at its own functions in that order.
    void UseBuildVTable()
    {
        if (DelegateInstance::UsesLayout420())
        {
            void* const** const VTablePointer = reinterpret_cast<void* const**>(this);
            *VTablePointer = DelegateInstance::BuildVTable<TBaseStaticDelegateInstance>(*VTablePointer);
        }
    }

    FFuncPtr StaticFuncPtr;
    std::tuple<VarTypes...> Payload;
    FDelegateHandle Handle;
};

// A delegate bound to a functor (a lambda), as TBaseDelegate::CreateLambda makes it. The engine relocates an instance
// with a memcpy when it moves a delegate (TInlineAllocator::MoveToEmpty up to 4.1), so the functor's captures must not
// point into themselves (no std::function; weak pointers, shared pointers, strings and vectors are fine).
template <typename FuncType, typename FunctorType>
class TBaseFunctorDelegateInstance;

template <typename RetType, typename... ParamTypes, typename FunctorType>
class TBaseFunctorDelegateInstance<RetType(ParamTypes...), FunctorType> : public IBaseDelegateInstance<RetType(ParamTypes...)>
{
public:
    explicit TBaseFunctorDelegateInstance(const FunctorType& InFunctor)
        : Functor(InFunctor), Handle(FDelegateHandle::GenerateNewHandle)
    {
        UseBuildVTable();
    }

    TBaseFunctorDelegateInstance(const TBaseFunctorDelegateInstance& Other)
        : Functor(Other.Functor), Handle(Other.Handle)
    {
        UseBuildVTable();
    }

    FName GetFunctionName() const final { return FName(); }
    UObject* GetUObject() const final { return nullptr; }
    const void* GetRawMethodPtr() const final { return nullptr; }
    const void* GetRawUserObject() const final { return nullptr; }
    EDelegateInstanceType::Type GetType() const final { return EDelegateInstanceType::Functor; }
    bool HasSameObject(const void*) const final { return false; }
    // Not IDelegateInstance's !IsSafeToExecute(): that is a virtual call, and 4.20's vtable has Execute at its slot.
    bool IsCompactable() const final { return !TBaseFunctorDelegateInstance::IsSafeToExecute(); }
    bool IsSafeToExecute() const final { return true; }
    FDelegateHandle GetHandle() const final { return Handle; }

    void CreateCopy(FDelegateBase& Base) final
    {
        new (Base) TBaseFunctorDelegateInstance(*this);
    }

    // Functors aren't comparable (Epic's TBaseFunctorDelegateInstance::IsSameFunction returns false too).
    bool IsSameFunction(const IBaseDelegateInstance<RetType(ParamTypes...)>&) const final
    {
        return false;
    }

    RetType Execute(ParamTypes... Params) const final
    {
        return Functor(Params...);
    }

    bool ExecuteIfSafe(ParamTypes... Params) const final
    {
        TBaseFunctorDelegateInstance::Execute(Params...);
        return true;
    }

private:
    void UseBuildVTable()
    {
        if (DelegateInstance::UsesLayout420())
        {
            void* const** const VTablePointer = reinterpret_cast<void* const**>(this);
            *VTablePointer = DelegateInstance::BuildVTable<TBaseFunctorDelegateInstance>(*VTablePointer);
        }
    }

    mutable std::remove_const_t<FunctorType> Functor;
    FDelegateHandle Handle;
};

template <typename RetType, typename... ParamTypes>
class TBaseDelegate : public FDelegateBase
{
public:
    using FDelegateInstance = IBaseDelegateInstance<RetType(ParamTypes...)>;

    template <typename... VarTypes>
    static TBaseDelegate CreateStatic(typename TBaseStaticDelegateInstance<RetType(ParamTypes...), VarTypes...>::FFuncPtr InFunc, VarTypes... Vars)
    {
        TBaseDelegate Result;
        new (Result) TBaseStaticDelegateInstance<RetType(ParamTypes...), VarTypes...>(InFunc, Vars...);
        return Result;
    }

    template <typename FunctorType>
    static TBaseDelegate CreateLambda(FunctorType&& InFunctor)
    {
        TBaseDelegate Result;
        new (Result) TBaseFunctorDelegateInstance<RetType(ParamTypes...), std::remove_cvref_t<FunctorType>>(InFunctor);
        return Result;
    }

    // A copy of Other's instance (TBaseDelegate's copy constructor, through the instance's CreateCopy).
    void CopyFrom(const TBaseDelegate& Other)
    {
        if (FDelegateInstance* OtherInstance = Other.GetDelegateInstance())
        {
            DelegateInstance::CreateCopy(OtherInstance, *this);
        }
        else
        {
            Unbind();
        }
    }

    TBaseDelegate() = default;

    TBaseDelegate(TBaseDelegate&& Other) noexcept
    {
        if (FDelegateInstance* OtherInstance = Other.GetDelegateInstance())
        {
            DelegateInstance::CreateCopy(OtherInstance, *this);
            Other.Unbind();
        }
    }

    RetType Execute(ParamTypes... Params) const
    {
        return DelegateInstance::Execute(GetDelegateInstance(), Params...);
    }

    bool ExecuteIfBound(ParamTypes... Params) const
    {
        if (IsBound())
        {
            Execute(Params...);
            return true;
        }
        return false;
    }

private:
    FDelegateInstance* GetDelegateInstance() const
    {
        return static_cast<FDelegateInstance*>(GetDelegateInstanceProtected());
    }
};

template <typename... ParamTypes>
class TMulticastDelegate
{
public:
    using FDelegateInstance = IBaseDelegateInstance<void(ParamTypes...)>;

    TMulticastDelegate() = delete;

    void Broadcast(ParamTypes... Params) const
    {
        bool NeedsCompaction = false;

        ++InvocationListLockCount();
        for (int32_t InvocationListIndex = ArrayNum() - 1; InvocationListIndex >= 0; --InvocationListIndex)
        {
            FDelegateInstance* DelegateInstance = static_cast<FDelegateInstance*>(Invocation(InvocationListIndex).GetDelegateInstanceProtected());
            if (!DelegateInstance || !::DelegateInstance::ExecuteIfSafe(DelegateInstance, Params...))
            {
                NeedsCompaction = true;
            }
        }
        --InvocationListLockCount();

        if (NeedsCompaction)
        {
            const_cast<TMulticastDelegate*>(this)->CompactInvocationList();
        }
    }

private:
    // The engine's multicast delegate in either layout (see FDelegateBase). Up to 4.1 the invocation list is a
    // TArray<FDelegateBase, TInlineAllocator<1>>; 4.2's is a TArray<FDelegateBase> (UConsole::
    // RegisterConsoleAutoCompleteEntries at 0x4917F50: Data, ArrayNum +8, CompactionThreshold +0x10, lock count +0x14).
    struct alignas(16) FInlineInvocationList
    {
        struct alignas(16) FInvocationList
        {
            struct alignas(16) FAllocator
            {
                alignas(16) uint8_t InlineData[sizeof(FDelegateBase)];
                uint8_t* SecondaryData;
            };

            FAllocator Allocator;
            int32_t ArrayNum;
            int32_t ArrayMax;
        };

        FInvocationList InvocationList;
        int32_t CompactionThreshold;
        int32_t InvocationListLockCount;
    };

    struct FHeapInvocationList
    {
        uint8_t* Data;
        int32_t ArrayNum;
        int32_t ArrayMax;
        int32_t CompactionThreshold;
        int32_t InvocationListLockCount;
    };

    FInlineInvocationList& Inline() const { return *reinterpret_cast<FInlineInvocationList*>(const_cast<uint8_t*>(Storage)); }
    FHeapInvocationList& Heap() const { return *reinterpret_cast<FHeapInvocationList*>(const_cast<uint8_t*>(Storage)); }

    int32_t& ArrayNum() const { return DelegateBase::UsesHeapAllocator() ? Heap().ArrayNum : Inline().InvocationList.ArrayNum; }
    int32_t& CompactionThreshold() const { return DelegateBase::UsesHeapAllocator() ? Heap().CompactionThreshold : Inline().CompactionThreshold; }
    int32_t& InvocationListLockCount() const { return DelegateBase::UsesHeapAllocator() ? Heap().InvocationListLockCount : Inline().InvocationListLockCount; }

    uint8_t* GetData() const
    {
        if (DelegateBase::UsesHeapAllocator())
        {
            return Heap().Data;
        }
        typename FInlineInvocationList::FInvocationList::FAllocator& Allocator = Inline().InvocationList.Allocator;
        return Allocator.SecondaryData ? Allocator.SecondaryData : Allocator.InlineData;
    }

    // The build's elements are FDelegateBase::BuildSize() apart.
    FDelegateBase& Invocation(int32_t Index) const
    {
        return *reinterpret_cast<FDelegateBase*>(GetData() + Index * FDelegateBase::BuildSize());
    }

    void CompactInvocationList()
    {
        if (InvocationListLockCount() > 0)
        {
            return;
        }

        const size_t ElementSize = FDelegateBase::BuildSize();
        const int32_t OldNumItems = ArrayNum();
        for (int32_t Index = 0; Index < ArrayNum();)
        {
            FDelegateBase& DelegateBaseRef = Invocation(Index);
            IDelegateInstance* DelegateInstance = DelegateBaseRef.GetDelegateInstanceProtected();
            if (!DelegateInstance || ::DelegateInstance::IsCompactable(DelegateInstance))
            {
                DelegateBaseRef.~FDelegateBase();
                if (Index != ArrayNum() - 1)
                {
                    memcpy(static_cast<void*>(&DelegateBaseRef), &Invocation(ArrayNum() - 1), ElementSize);
                }
                --ArrayNum();
            }
            else
            {
                ++Index;
            }
        }

        CompactionThreshold() = ArrayNum() * 2 > 2 ? ArrayNum() * 2 : 2;
        if (DelegateBase::UsesHeapAllocator())
        {
            // InvocationList.Shrink() (4.2 0x6E94FE).
            FHeapInvocationList& List = Heap();
            if (OldNumItems > List.CompactionThreshold && List.ArrayMax != List.ArrayNum)
            {
                List.ArrayMax = List.ArrayNum;
                if (List.Data || List.ArrayNum)
                {
                    List.Data = static_cast<uint8_t*>(FMemory::Realloc(List.Data, List.ArrayNum * ElementSize));
                }
            }
            return;
        }

        typename FInlineInvocationList::FInvocationList& List = Inline().InvocationList;
        if (OldNumItems > Inline().CompactionThreshold && List.Allocator.SecondaryData && List.ArrayNum <= 1)
        {
            if (List.ArrayNum == 1)
            {
                memcpy(List.Allocator.InlineData, List.Allocator.SecondaryData, ElementSize);
            }
            FMemory::Free(List.Allocator.SecondaryData);
            List.Allocator.SecondaryData = nullptr;
            List.ArrayMax = 1;
        }
    }

    alignas(16) uint8_t Storage[sizeof(FInlineInvocationList)];

    friend struct FMulticastDelegateLayout;
};

struct FMulticastDelegateLayout
{
    using FInline = TMulticastDelegate<int32_t>::FInlineInvocationList;
    using FHeap = TMulticastDelegate<int32_t>::FHeapInvocationList;
    static_assert(offsetof(FInline, InvocationList.ArrayNum) == 0x50);
    static_assert(offsetof(FInline, CompactionThreshold) == 0x60);
    static_assert(offsetof(FInline, InvocationListLockCount) == 0x64);
    static_assert(sizeof(FInline) == 0x70);
    static_assert(offsetof(FHeap, ArrayNum) == 0x08);
    static_assert(offsetof(FHeap, CompactionThreshold) == 0x10);
    static_assert(offsetof(FHeap, InvocationListLockCount) == 0x14);
};
