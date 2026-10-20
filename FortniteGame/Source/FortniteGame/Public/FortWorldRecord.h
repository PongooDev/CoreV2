#pragma once

#include "Containers/UnrealString.h"
#include "StructArray.h"
#include "UObject/Object.h"

struct FFortPlayerRecord
{
    DefineUnrealStruct(FFortPlayerRecord, FortniteGame)
    DefineUProperty(FString, DisplayName)
    DefineUProperty(FString, UniqueId)
    DefineBitfieldUProperty(bPlayerIsNew)
};

class UFortWorldRecord : public UObject
{
    DefineUnrealClass(UFortWorldRecord, FortniteGame)

public:
    DefineUStructArrayProperty(FFortPlayerRecord, SavedPlayers)
};
