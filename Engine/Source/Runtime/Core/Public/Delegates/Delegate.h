#pragma once

// Epic's Delegate.h: the native delegate types, from the headers Epic keeps them in.
//   IDelegateInstance.h          IDelegateInstance, FDelegateHandle, the build's instance vtable layout
//   DelegateBase.h               FDelegateBase (a delegate's instance storage, in the build's allocator layout)
//   DelegateInstanceInterface.h  IBaseDelegateInstance
//   DelegateInstancesImpl.h      the static, functor (lambda) and UObject method instances Core binds with
//   MulticastDelegateBase.h      FMulticastDelegateBase (the invocation list: add, remove, compact)
//   DelegateSignatureImpl.inl    TBaseDelegate (DECLARE_DELEGATE) and TMulticastDelegate (DECLARE_MULTICAST_DELEGATE)
// The dynamic (Blueprint) delegates are CoreUObject's: UObject/ScriptDelegates.h.
#include "Delegates/IDelegateInstance.h"
#include "Delegates/DelegateBase.h"
#include "Delegates/DelegateInstanceInterface.h"
#include "Delegates/DelegateInstancesImpl.h"
#include "Delegates/MulticastDelegateBase.h"
#include "Delegates/DelegateSignatureImpl.inl"
