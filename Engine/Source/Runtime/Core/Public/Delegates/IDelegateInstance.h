#pragma once

#include "ObjectLayout.h"
#include "UObject/NameTypes.h"

#include <atomic>
#include <cstdint>
#include <iterator>

class UObject;

namespace EDelegateInstanceType
{
    enum Type
    {
        SharedPointerMethod,
        ThreadSafeSharedPointerMethod,
        RawMethod,
        UFunction,
        UObjectMethod,
        Raw,
        Functor,
    };
}

class FDelegateHandle
{
public:
    enum EGenerateNewHandleType
    {
        GenerateNewHandle
    };

    FDelegateHandle() = default;

    explicit FDelegateHandle(EGenerateNewHandleType)
        : ID(GenerateNewID())
    {
    }

    bool IsValid() const
    {
        return ID != 0;
    }

    bool operator==(const FDelegateHandle& Other) const = default;

private:
    static uint64_t GenerateNewID()
    {
        static std::atomic<uint64_t> NextID = 1;
        uint64_t Result = NextID++;
        if (Result == 0)
        {
            Result = NextID++;
        }
        return Result;
    }

    uint64_t ID = 0;
};

class IDelegateInstance
{
public:
    virtual FName GetFunctionName() const = 0;
    virtual UObject* GetUObject() const = 0;
    virtual const void* GetRawMethodPtr() const = 0;
    virtual const void* GetRawUserObject() const = 0;
    virtual EDelegateInstanceType::Type GetType() const = 0;
    virtual bool HasSameObject(const void* InUserObject) const = 0;

    virtual bool IsCompactable() const
    {
        return !IsSafeToExecute();
    }

    virtual bool IsSafeToExecute() const = 0;
    virtual FDelegateHandle GetHandle() const = 0;

    virtual ~IDelegateInstance() = default;
};

// The build's delegate instance vtable. 1.1-2.4.2 (UE 4.16-4.19) lay it out as the classes here declare it. UE 4.20 (2.5)
// drops GetFunctionName (TryGetBoundFunctionName, compiled out of shipping), GetRawMethodPtr, GetRawUserObject, GetType
// and IBaseDelegateInstance::IsSameFunction: 2.5's static instance vtable (0x30C79F8) is GetUObject, HasSameObject,
// IsCompactable, IsSafeToExecute, GetHandle, the destructor, CreateCopy, Execute, ExecuteIfSafe. Core calls an instance's
// virtuals by the build's slots, and an instance Core makes takes a vtable in the build's order (BuildVTable), so the
// engine and Core agree on both sides (#192: on 2.5, Core's console autocomplete called the engine's
// RegisterConsoleAutoCompleteEntries instances at slot 13, past their vtable, and the console crashed when opened).
enum class EDelegateInstanceSlot : int32_t
{
    GetUObject,
    HasSameObject,
    IsCompactable,
    IsSafeToExecute,
    GetHandle,
    Destructor,
    CreateCopy,
    Execute,
    ExecuteIfSafe,
    Num,
};

namespace DelegateInstance
{
    // The slots of 4.16-4.19's vtable, in EDelegateInstanceSlot order; 4.20's are 0-8.
    inline constexpr int32_t SlotsBefore420[] = { 1, 5, 6, 7, 8, 9, 10, 12, 13 };
    static_assert(std::size(SlotsBefore420) == static_cast<size_t>(EDelegateInstanceSlot::Num));

    // 5.30 (CL-4305896) adds GetObjectForTimerManager after GetUObject (21.40's IDelegateInstance has it there), so
    // everything after GetUObject moves down one: its static instance vtable (0x38B0C70) is GetUObject, GetObjectFor-
    // TimerManager, HasSameObject, IsCompactable, IsSafeToExecute, GetHandle (slot 5), the destructor, CreateCopy, Execute,
    // ExecuteIfSafe.
    inline constexpr int32_t GetObjectForTimerManagerSlot = 1;
    inline constexpr int32_t SlotsWithTimerManagerObject[] = { 0, 2, 3, 4, 5, 6, 7, 8, 9 };
    static_assert(std::size(SlotsWithTimerManagerObject) == static_cast<size_t>(EDelegateInstanceSlot::Num));

    inline bool UsesTimerManagerObjectLayout()
    {
        return Natives::IDelegateInstance_GetHandleIndex == SlotsWithTimerManagerObject[static_cast<int32_t>(EDelegateInstanceSlot::GetHandle)];
    }

    // 4.20's order, with or without GetObjectForTimerManager: Core builds its instances' vtables in the build's order.
    inline bool UsesLayout420()
    {
        return Natives::IDelegateInstance_GetHandleIndex == static_cast<int32_t>(EDelegateInstanceSlot::GetHandle) || UsesTimerManagerObjectLayout();
    }

    inline int32_t Slot(EDelegateInstanceSlot Function)
    {
        if (UsesTimerManagerObjectLayout())
        {
            return SlotsWithTimerManagerObject[static_cast<int32_t>(Function)];
        }
        return UsesLayout420() ? static_cast<int32_t>(Function) : SlotsBefore420[static_cast<int32_t>(Function)];
    }

    // A vtable in 4.20's order (or 5.30's, with GetObjectForTimerManager) taken from Core's own (4.16-4.19) one, built
    // once per instance type. A static instance has no object for the timer manager, as it has no UObject: Core's
    // GetUObject (nullptr) answers both.
    template <typename InstanceType>
    void* const* BuildVTable(void* const* CompiledVTable)
    {
        static void* VTable[static_cast<size_t>(EDelegateInstanceSlot::Num) + 1] = {};
        static const bool bBuilt = [CompiledVTable]
        {
            const bool bTimerManagerObject = UsesTimerManagerObjectLayout();
            for (size_t Index = 0; Index < static_cast<size_t>(EDelegateInstanceSlot::Num); ++Index)
            {
                VTable[bTimerManagerObject ? SlotsWithTimerManagerObject[Index] : Index] = CompiledVTable[SlotsBefore420[Index]];
            }
            if (bTimerManagerObject)
            {
                VTable[GetObjectForTimerManagerSlot] = CompiledVTable[SlotsBefore420[static_cast<int32_t>(EDelegateInstanceSlot::GetUObject)]];
            }
            return true;
        }();
        (void)bBuilt;
        return VTable;
    }

    inline bool IsCompactable(const IDelegateInstance* Instance)
    {
        return CallVirtual<bool>(Instance, Slot(EDelegateInstanceSlot::IsCompactable));
    }

    inline bool IsSafeToExecute(const IDelegateInstance* Instance)
    {
        return CallVirtual<bool>(Instance, Slot(EDelegateInstanceSlot::IsSafeToExecute));
    }

    inline FDelegateHandle GetHandle(const IDelegateInstance* Instance)
    {
        FDelegateHandle Result;
        CallVirtual<FDelegateHandle*>(Instance, Slot(EDelegateInstanceSlot::GetHandle), &Result);
        return Result;
    }

    // The scalar deleting destructor with no delete flag: the instance lives in its FDelegateBase's allocation.
    inline void Destroy(IDelegateInstance* Instance)
    {
        CallVirtual<void, IDelegateInstance, uint32_t>(Instance, Slot(EDelegateInstanceSlot::Destructor), 0);
    }
}
