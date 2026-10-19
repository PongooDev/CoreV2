#pragma once

#include "UObject/NameTypes.h"

#include <atomic>

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
