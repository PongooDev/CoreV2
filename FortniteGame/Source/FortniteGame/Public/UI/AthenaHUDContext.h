#pragma once

#include "UObject/NameTypes.h"
#include "UObject/Object.h"

#include <cstdint>

class UAthenaHUDContext : public UObject
{
    DefineUnrealClass(UAthenaHUDContext, FortniteUI)
    DefineUFunction(HandleUIGameplayCue)

public:
    // EGameplayCueEvent::Type; the cue's parameters are left at their defaults.
    void HandleUIGameplayCue(FName CueName, uint8_t EventType);

    void BindLocalPawn();

    static void Hook();

private:
    static inline void (*BindLocalPawnOG)(UAthenaHUDContext* This) = nullptr;
    static void BindLocalPawnHK(UAthenaHUDContext* This);
};
