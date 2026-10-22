#pragma once

#include "UObject/Object.h"

class EFortCustomPartType
{
    DefineUnrealEnum(EFortCustomPartType, FortniteGame)
    DefineUEnumValue(Head)
    DefineUEnumValue(Body)
    DefineUEnumValue(Hat)
    DefineUEnumValue(Backpack)
    DefineUEnumValue(Charm)
    DefineUEnumValue(Face)
    DefineUEnumValue(NumTypes)
};

class UCustomCharacterPart : public UObject
{
    DefineUnrealClass(UCustomCharacterPart, FortniteGame)

public:
    DefineUProperty(EFortCustomPartType, CharacterPartType)
};
