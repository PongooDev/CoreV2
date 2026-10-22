#pragma once

#include "Components/ActorComponent.h"

enum class EFortVisibilityBehavior : uint8_t
{
    Proximity,
    OnceSeenAlwaysSeen,
    Invalid,
};

class UFortVisibilityComponent : public UActorComponent
{
    DefineUnrealClass(UFortVisibilityComponent, FortniteGame)

public:
    DefineBitfieldUProperty(bRegisterWithVisibilityManager)
    DefineBitfieldUProperty(bObserver)
    DefineUProperty(EFortVisibilityBehavior, VisibilityBehavior)
    DefineUProperty(float, Radius)

    void BeginPlay();
    void EndPlay(EEndPlayReason::Type EndPlayReason);

    static void HookVTables();

private:
    static void BeginPlayHK(UFortVisibilityComponent* This);
    static void EndPlayHK(UFortVisibilityComponent* This, EEndPlayReason::Type EndPlayReason);
    static inline void (*BeginPlayOG)(UFortVisibilityComponent* This) = nullptr;
    static inline void (*EndPlayOG)(UFortVisibilityComponent* This, EEndPlayReason::Type EndPlayReason) = nullptr;
};
