#pragma once

#include "UObject/Object.h"

enum class EFortCustomPartType : uint8_t
{
    Head,
    Body,
    Hat,
    Backpack,
    Charm,
    Face,
    NumTypes,
};

class UCustomCharacterPart : public UObject
{
    DefineUnrealClass(UCustomCharacterPart, FortniteGame)

public:
    DefineUProperty(EFortCustomPartType, CharacterPartType)
};
