#pragma once

#include "ObjectLayout.h"
#include "UObject/Object.h"

class UWorld;

class ULevel : public UObject
{
    DefineUnrealClass(ULevel, Engine)

public:
    DefineUProperty(UWorld*, OwningWorld)
    DefineBitfieldUProperty(bIsVisible)
    DefineNativeBitfield(bIsAssociatingLevel, ULevel_bIsAssociatingLevel, Natives::ULevel_bIsAssociatingLevelMask)
};
