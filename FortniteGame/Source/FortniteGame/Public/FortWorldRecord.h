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
    // 7.40 (CL-5046157) drops them: the layout is the world profile's loadout and the controller's cache.
    static bool HasQuickBarRecords() { return Member_PrimaryQuickBarRecord.IsAvailable() && Member_SecondaryQuickBarRecord.IsAvailable(); }
};

class UFortWorldRecord : public UObject
{
    DefineUnrealClass(UFortWorldRecord, FortniteGame)

public:
    DefineUStructArrayProperty(FFortPlayerRecord, SavedPlayers)
};
