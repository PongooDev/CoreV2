#pragma once

#include "Containers/UnrealString.h"
#include "Items/FortQuickBars.h"
#include "StructArray.h"
#include "UObject/Object.h"

struct FFortPlayerRecord
{
    DefineUnrealStruct(FFortPlayerRecord, FortniteGame)
    DefineUProperty(FString, DisplayName)
    DefineUProperty(FString, UniqueId)
    DefineBitfieldUProperty(bPlayerIsNew)
    DefineUStructProperty(FQuickBar, PrimaryQuickBarRecord)
    DefineUStructProperty(FQuickBar, SecondaryQuickBarRecord)
};

class UFortWorldRecord : public UObject
{
    DefineUnrealClass(UFortWorldRecord, FortniteGame)

public:
    DefineUStructArrayProperty(FFortPlayerRecord, SavedPlayers)
};
