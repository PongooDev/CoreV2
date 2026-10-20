#pragma once

#include "Containers/Array.h"
#include "UObject/Object.h"

class UCustomCharacterPart;

class UFortHero : public UObject
{
    DefineUnrealClass(UFortHero, FortniteGame)

public:
    DefineUProperty(TArray<UCustomCharacterPart*>, CharacterParts)
};
