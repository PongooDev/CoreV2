#pragma once

#include "Delegates/DelegateBase.h"
#include "Delegates/DelegateInstanceInterface.h"
#include "UObject/WeakObjectPtr.h"

#include <tuple>
#include <type_traits>
#include <utility>

// The delegate instances Core binds with, Epic's (DelegateInstancesImpl.h): the engine calls them through the build's
// slots (UseBuildVTable), so they sit in engine delegates as the engine's own do. Their payloads and lambda captures are
// relocated bitwise, as UE relocates every delegate (FDelegateBase moves, invocation list growth): nothing that points
// into itself (std::function, a self pointer). Each instance's functions call each other non-virtually (Class::Function):
// on 4.20+ its vtable is in the build's order, not the compiler's.

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
        DelegateInstance::UseBuildVTable(this);
    }

    TBaseStaticDelegateInstance(const TBaseStaticDelegateInstance& Other)
        : StaticFuncPtr(Other.StaticFuncPtr), Payload(Other.Payload), Handle(Other.Handle)
    {
        DelegateInstance::UseBuildVTable(this);
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

    // Only 4.16-4.19 have it in their vtable, where Core's compiled order is the build's, so Other's virtuals are too.
    bool IsSameFunction(const IBaseDelegateInstance<RetType(ParamTypes...)>& InOtherDelegate) const final
    {
        return InOtherDelegate.GetType() == EDelegateInstanceType::Raw && InOtherDelegate.GetRawMethodPtr() == TBaseStaticDelegateInstance::GetRawMethodPtr();
    }

    RetType Execute(ParamTypes... Params) const final
    {
        return std::apply([&](auto&... Vars) -> RetType { return StaticFuncPtr(Params..., Vars...); }, Payload);
    }

    bool ExecuteIfSafe(ParamTypes... Params) const final
    {
        TBaseStaticDelegateInstance::Execute(Params...);
        return true;
    }

private:
    FFuncPtr StaticFuncPtr;
    std::tuple<VarTypes...> Payload;
    FDelegateHandle Handle;
};

// A lambda (CreateLambda, AddLambda): always safe to execute, bound to no object.
template <typename FuncType, typename FunctorType, typename... VarTypes>
class TBaseFunctorDelegateInstance;

template <typename RetType, typename... ParamTypes, typename FunctorType, typename... VarTypes>
class TBaseFunctorDelegateInstance<RetType(ParamTypes...), FunctorType, VarTypes...> : public IBaseDelegateInstance<RetType(ParamTypes...)>
{
public:
    TBaseFunctorDelegateInstance(const FunctorType& InFunctor, VarTypes... Vars)
        : Functor(InFunctor), Payload(Vars...), Handle(FDelegateHandle::GenerateNewHandle)
    {
        DelegateInstance::UseBuildVTable(this);
    }

    TBaseFunctorDelegateInstance(FunctorType&& InFunctor, VarTypes... Vars)
        : Functor(std::move(InFunctor)), Payload(Vars...), Handle(FDelegateHandle::GenerateNewHandle)
    {
        DelegateInstance::UseBuildVTable(this);
    }

    TBaseFunctorDelegateInstance(const TBaseFunctorDelegateInstance& Other)
        : Functor(Other.Functor), Payload(Other.Payload), Handle(Other.Handle)
    {
        DelegateInstance::UseBuildVTable(this);
    }

    FName GetFunctionName() const final { return FName(); }
    UObject* GetUObject() const final { return nullptr; }
    const void* GetRawMethodPtr() const final { return nullptr; }
    const void* GetRawUserObject() const final { return nullptr; }
    EDelegateInstanceType::Type GetType() const final { return EDelegateInstanceType::Functor; }
    bool HasSameObject(const void*) const final { return false; }
    bool IsCompactable() const final { return !TBaseFunctorDelegateInstance::IsSafeToExecute(); }
    bool IsSafeToExecute() const final { return true; }
    FDelegateHandle GetHandle() const final { return Handle; }

    void CreateCopy(FDelegateBase& Base) final
    {
        new (Base) TBaseFunctorDelegateInstance(*this);
    }

    bool IsSameFunction(const IBaseDelegateInstance<RetType(ParamTypes...)>&) const final
    {
        return false;
    }

    RetType Execute(ParamTypes... Params) const final
    {
        return std::apply([&](auto&... Vars) -> RetType { return Functor(Params..., Vars...); }, Payload);
    }

    bool ExecuteIfSafe(ParamTypes... Params) const final
    {
        TBaseFunctorDelegateInstance::Execute(Params...);
        return true;
    }

private:
    // Mutable, as Epic's: a mutable lambda can be bound and executed through the const Execute.
    mutable std::remove_const_t<FunctorType> Functor;
    std::tuple<VarTypes...> Payload;
    FDelegateHandle Handle;
};

// A UObject's member function (CreateUObject, AddUObject), held by a weak pointer: it runs only while the object lives,
// and a multicast delegate compacts it out once the object is gone. UserClass is Core's view of the object's class.
template <bool bConst, class UserClass, typename FuncType, typename... VarTypes>
class TBaseUObjectMethodDelegateInstance;

template <bool bConst, class UserClass, typename RetType, typename... ParamTypes, typename... VarTypes>
class TBaseUObjectMethodDelegateInstance<bConst, UserClass, RetType(ParamTypes...), VarTypes...> : public IBaseDelegateInstance<RetType(ParamTypes...)>
{
public:
    using FMethodPtr = std::conditional_t<bConst, RetType (UserClass::*)(ParamTypes..., VarTypes...) const, RetType (UserClass::*)(ParamTypes..., VarTypes...)>;
    using FUserObjectPtr = std::conditional_t<bConst, const UserClass*, UserClass*>;

    TBaseUObjectMethodDelegateInstance(FUserObjectPtr InUserObject, FMethodPtr InMethodPtr, VarTypes... Vars)
        : UserObject(InUserObject), MethodPtr(InMethodPtr), Payload(Vars...), Handle(FDelegateHandle::GenerateNewHandle)
    {
        DelegateInstance::UseBuildVTable(this);
    }

    TBaseUObjectMethodDelegateInstance(const TBaseUObjectMethodDelegateInstance& Other)
        : UserObject(Other.UserObject), MethodPtr(Other.MethodPtr), Payload(Other.Payload), Handle(Other.Handle)
    {
        DelegateInstance::UseBuildVTable(this);
    }

    FName GetFunctionName() const final { return FName(); }
    // 5.30+'s GetObjectForTimerManager takes this one's slot too (BuildVTable), as 21.40's returns the user object.
    UObject* GetUObject() const final { return reinterpret_cast<UObject*>(UserObject.Get()); }
    const void* GetRawMethodPtr() const final { return *reinterpret_cast<const void* const*>(&MethodPtr); }
    const void* GetRawUserObject() const final { return UserObject.Get(); }
    EDelegateInstanceType::Type GetType() const final { return EDelegateInstanceType::UObjectMethod; }
    // Epic's UserObject.HasSameIndexAndSerialNumber(InUserObject): the object, pending kill or not.
    bool HasSameObject(const void* InUserObject) const final { return InUserObject && UserObject.Get(true) == InUserObject; }
    bool IsCompactable() const final { return !UserObject.Get(true); }
    bool IsSafeToExecute() const final { return UserObject.Get() != nullptr; }
    FDelegateHandle GetHandle() const final { return Handle; }

    void CreateCopy(FDelegateBase& Base) final
    {
        new (Base) TBaseUObjectMethodDelegateInstance(*this);
    }

    // Only 4.16-4.19 have it in their vtable, where Core's compiled order is the build's, so Other's virtuals are too.
    bool IsSameFunction(const IBaseDelegateInstance<RetType(ParamTypes...)>& InOtherDelegate) const final
    {
        return InOtherDelegate.GetType() == EDelegateInstanceType::UObjectMethod
            && InOtherDelegate.GetRawMethodPtr() == TBaseUObjectMethodDelegateInstance::GetRawMethodPtr()
            && InOtherDelegate.GetRawUserObject() == TBaseUObjectMethodDelegateInstance::GetRawUserObject();
    }

    RetType Execute(ParamTypes... Params) const final
    {
        const FUserObjectPtr ActualUserObject = UserObject.Get();
        return std::apply([&](auto&... Vars) -> RetType { return (ActualUserObject->*MethodPtr)(Params..., Vars...); }, Payload);
    }

    bool ExecuteIfSafe(ParamTypes... Params) const final
    {
        if (UserObject.Get())
        {
            TBaseUObjectMethodDelegateInstance::Execute(Params...);
            return true;
        }
        return false;
    }

private:
    TWeakObjectPtr<std::remove_const_t<UserClass>> UserObject;
    FMethodPtr MethodPtr;
    std::tuple<VarTypes...> Payload;
    FDelegateHandle Handle;
};
