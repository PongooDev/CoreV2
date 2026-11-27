#pragma once

#include "Containers/Array.h"
#include "ObjectLayout.h"
#include "UObject/Object.h"

class AActor;
class UWorld;

class ULevel : public UObject
{
    DefineUnrealClass(ULevel, Engine)

public:
    DefineUProperty(UWorld*, OwningWorld)
    DefineBitfieldUProperty(bIsVisible)
    static bool HasIsVisible()
    {
        return Member_bIsVisible.IsAvailable();
    }
    DefineNativeBitfield(bIsAssociatingLevel, ULevel_bIsAssociatingLevel, Natives::ULevel_bIsAssociatingLevelMask)

    bool HasVisibilityChangeRequestPending() const;
    TArray<AActor*>& GetActors() const;
};
