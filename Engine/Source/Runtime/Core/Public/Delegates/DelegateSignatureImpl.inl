#pragma once

#include "Delegates/DelegateInstancesImpl.h"
#include "Delegates/MulticastDelegateBase.h"
#include "Log.h"

#include <type_traits>
#include <utility>

// Epic's TBaseDelegate (DECLARE_DELEGATE's type): one instance, in the build's FDelegateBase layout.
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

    template <typename FunctorType, typename... VarTypes>
    static TBaseDelegate CreateLambda(FunctorType&& InFunctor, VarTypes... Vars)
    {
        TBaseDelegate Result;
        new (Result) TBaseFunctorDelegateInstance<RetType(ParamTypes...), std::remove_cvref_t<FunctorType>, VarTypes...>(std::forward<FunctorType>(InFunctor), Vars...);
        return Result;
    }

    // InUserObject's member function, while the object lives (UserClass: Core's view of its class).
    template <typename UserClass, typename... VarTypes>
    static TBaseDelegate CreateUObject(UserClass* InUserObject, typename TBaseUObjectMethodDelegateInstance<false, UserClass, RetType(ParamTypes...), VarTypes...>::FMethodPtr InFunc, VarTypes... Vars)
    {
        TBaseDelegate Result;
        new (Result) TBaseUObjectMethodDelegateInstance<false, UserClass, RetType(ParamTypes...), VarTypes...>(InUserObject, InFunc, Vars...);
        return Result;
    }

    template <typename UserClass, typename... VarTypes>
    static TBaseDelegate CreateUObject(const UserClass* InUserObject, typename TBaseUObjectMethodDelegateInstance<true, UserClass, RetType(ParamTypes...), VarTypes...>::FMethodPtr InFunc, VarTypes... Vars)
    {
        TBaseDelegate Result;
        new (Result) TBaseUObjectMethodDelegateInstance<true, UserClass, RetType(ParamTypes...), VarTypes...>(InUserObject, InFunc, Vars...);
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

// Epic's TMulticastDelegate (DECLARE_MULTICAST_DELEGATE's type), in the build's layout (FMulticastDelegateBase). Core
// reaches an engine delegate as one of these and binds to it the engine's way:
//
//     FDelegateHandle Handle = Delegate.AddUObject(PlayerController, &AFortPlayerController::OnSomething);
//     Delegate.Remove(Handle);   // or RemoveAll(PlayerController)
//
// AddStatic, AddLambda and AddUObject return an invalid handle and bind nothing where the build's delegate instance
// layout wasn't read (Natives::IDelegateInstance_GetHandleIndexResolved): the engine would call Core's instance by the
// wrong slots. ParamTypes must be the engine's exactly (same sizes, references where it takes references).
template <typename... ParamTypes>
class TMulticastDelegate : public FMulticastDelegateBase
{
public:
    using FDelegate = TBaseDelegate<void, ParamTypes...>;
    using FDelegateInstance = IBaseDelegateInstance<void(ParamTypes...)>;

    TMulticastDelegate() = default;

    // Adds a bound delegate's instance (it moves into the list). Returns its handle, for Remove.
    FDelegateHandle Add(FDelegate&& InNewDelegate)
    {
        FDelegateHandle Result;
        if (!DelegateInstance::IsLayoutResolved())
        {
            ReportUnresolvedLayout();
            return Result;
        }
        if (InNewDelegate.GetDelegateInstanceProtected())
        {
            Result = AddInternal(std::move(InNewDelegate));
        }
        return Result;
    }

    template <typename... VarTypes>
    FDelegateHandle AddStatic(typename TBaseStaticDelegateInstance<void(ParamTypes...), VarTypes...>::FFuncPtr InFunc, VarTypes... Vars)
    {
        return DelegateInstance::IsLayoutResolved() ? Add(FDelegate::CreateStatic(InFunc, Vars...)) : Add(FDelegate());
    }

    template <typename FunctorType, typename... VarTypes>
    FDelegateHandle AddLambda(FunctorType&& InFunctor, VarTypes... Vars)
    {
        return DelegateInstance::IsLayoutResolved() ? Add(FDelegate::CreateLambda(std::forward<FunctorType>(InFunctor), Vars...)) : Add(FDelegate());
    }

    template <typename UserClass, typename... VarTypes>
    FDelegateHandle AddUObject(UserClass* InUserObject, typename TBaseUObjectMethodDelegateInstance<false, UserClass, void(ParamTypes...), VarTypes...>::FMethodPtr InFunc, VarTypes... Vars)
    {
        return DelegateInstance::IsLayoutResolved() ? Add(FDelegate::CreateUObject(InUserObject, InFunc, Vars...)) : Add(FDelegate());
    }

    template <typename UserClass, typename... VarTypes>
    FDelegateHandle AddUObject(const UserClass* InUserObject, typename TBaseUObjectMethodDelegateInstance<true, UserClass, void(ParamTypes...), VarTypes...>::FMethodPtr InFunc, VarTypes... Vars)
    {
        return DelegateInstance::IsLayoutResolved() ? Add(FDelegate::CreateUObject(InUserObject, InFunc, Vars...)) : Add(FDelegate());
    }

    // Removes the instance Add gave Handle for (it is unbound at once, and compacted out unless a broadcast holds the list).
    bool Remove(FDelegateHandle Handle)
    {
        return Handle.IsValid() && RemoveDelegateInstance(Handle);
    }

    // Calls every bound instance, last added first, so ones added during the broadcast aren't called. Instances that
    // aren't safe to execute (a UObject's that is gone) are compacted out afterwards.
    void Broadcast(ParamTypes... Params) const
    {
        bool NeedsCompaction = false;

        LockInvocationList();
        for (int32_t InvocationListIndex = GetInvocationListNum() - 1; InvocationListIndex >= 0; --InvocationListIndex)
        {
            FDelegateInstance* Instance = static_cast<FDelegateInstance*>(Invocation(InvocationListIndex).GetDelegateInstanceProtected());
            if (!Instance || !DelegateInstance::ExecuteIfSafe(Instance, Params...))
            {
                NeedsCompaction = true;
            }
        }
        UnlockInvocationList();

        if (NeedsCompaction)
        {
            const_cast<TMulticastDelegate*>(this)->CompactInvocationList();
        }
    }

private:
    static void ReportUnresolvedLayout()
    {
        static bool bReported = false;
        if (!bReported)
        {
            bReported = true;
            Log::Error(L"Delegates: the build's delegate instance layout wasn't read, so Core binds nothing to its native delegates");
        }
    }
};
