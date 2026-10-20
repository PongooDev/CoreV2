#pragma once

class UObject;

class FScriptInterface
{
public:
    FScriptInterface() = default;

    FScriptInterface(UObject* InObjectPointer, void* InInterfacePointer)
        : ObjectPointer(InObjectPointer), InterfacePointer(InInterfacePointer)
    {
    }

    UObject* GetObject() const { return ObjectPointer; }
    void* GetInterface() const { return InterfacePointer; }

private:
    UObject* ObjectPointer = nullptr;
    void* InterfacePointer = nullptr;
};

template <class InterfaceType>
class TScriptInterface : public FScriptInterface
{
public:
    TScriptInterface() = default;

    TScriptInterface(UObject* InObjectPointer, InterfaceType* InInterfacePointer)
        : FScriptInterface(InObjectPointer, InInterfacePointer)
    {
    }

    InterfaceType* operator->() const { return static_cast<InterfaceType*>(GetInterface()); }
};

static_assert(sizeof(FScriptInterface) == 0x10);
